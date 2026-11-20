/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#define _POSIX_C_SOURCE 200809L

#include "openst133_spe.h"
#include "test_check.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define OUTPUT_CAPACITY 65536u

struct result {
    int exit_code;
    char output[OUTPUT_CAPACITY];
    char error[OUTPUT_CAPACITY];
};

static void put16(unsigned char *buffer, size_t offset, uint16_t value)
{
    buffer[offset] = (unsigned char)(value & 0xffu);
    buffer[offset + 1] = (unsigned char)(value >> 8);
}

static void put32(unsigned char *buffer, size_t offset, uint32_t value)
{
    size_t i;

    for (i = 0u; i < 4u; ++i) {
        buffer[offset + i] = (unsigned char)(value >> (i * 8u));
    }
}

static void put_float(unsigned char *buffer, size_t offset, float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));
    put32(buffer, offset, bits);
}

static void write_fixture(const char *path)
{
    unsigned char bytes[OST_SPE_HEADER_SIZE + 8u] = {0};
    static const unsigned char pixels[8] = {
        1, 0, 2, 0, 44, 1, 255, 255,
    };
    FILE *stream;

    put16(bytes, 42u, 2u);
    put16(bytes, 108u, OST_SPE_UINT16);
    put16(bytes, 656u, 1u);
    put32(bytes, 1446u, 2u);
    put_float(bytes, 10u, 0.25f);
    put_float(bytes, 1992u, 2.5f);
    memcpy(bytes + 20u, "01Jan2020", 10u);
    memcpy(bytes + 688u, "test \"reader\"", 13u);
    memcpy(bytes + OST_SPE_HEADER_SIZE, pixels, sizeof(pixels));
    stream = fopen(path, "wb");
    TEST_CHECK(stream != NULL);
    TEST_CHECK(fwrite(bytes, 1u, sizeof(bytes), stream) == sizeof(bytes));
    TEST_CHECK(fclose(stream) == 0);
}

#ifdef _WIN32
static void read_result_file(const char *path, char *output, size_t capacity)
{
    FILE *stream = fopen(path, "rb");
    size_t used;

    TEST_CHECK(stream != NULL);
    used = fread(output, 1u, capacity - 1u, stream);
    TEST_CHECK(!ferror(stream) && feof(stream));
    TEST_CHECK(fclose(stream) == 0);
    TEST_CHECK(remove(path) == 0);
    output[used] = '\0';
}

static void run_ost(const char *executable, struct result *result, int argc, ...)
{
    char command[16384];
    char stdout_path[512];
    char stderr_path[512];
    const char *temporary = getenv("TEMP");
    size_t used;
    int status;
    int i;
    va_list values;

    if (temporary == NULL || temporary[0] == '\0') {
        temporary = ".";
    }
    TEST_CHECK(snprintf(stdout_path, sizeof(stdout_path), "%s/ost-cli-%ld.stdout",
        temporary, (long)_getpid()) > 0);
    TEST_CHECK(snprintf(stderr_path, sizeof(stderr_path), "%s/ost-cli-%ld.stderr",
        temporary, (long)_getpid()) > 0);
    used = (size_t)snprintf(command, sizeof(command), "\"%s\"", executable);
    TEST_CHECK(used < sizeof(command));
    va_start(values, argc);
    for (i = 0; i < argc; ++i) {
        const char *argument = va_arg(values, const char *);
        int written = snprintf(command + used, sizeof(command) - used, " \"%s\"", argument);

        TEST_CHECK(written > 0 && (size_t)written < sizeof(command) - used);
        used += (size_t)written;
    }
    va_end(values);
    TEST_CHECK(snprintf(command + used, sizeof(command) - used, " > \"%s\" 2> \"%s\"",
        stdout_path, stderr_path) > 0);
    status = system(command);
    TEST_CHECK(status >= 0);
    result->exit_code = status;
    read_result_file(stdout_path, result->output, sizeof(result->output));
    read_result_file(stderr_path, result->error, sizeof(result->error));
}
#else
static void run_ost(const char *executable, struct result *result, int argc, ...)
{
    char *arguments[16];
    int stdout_pipe[2];
    int stderr_pipe[2];
    pid_t child;
    size_t total_stdout = 0u;
    size_t total_stderr = 0u;
    ssize_t count;
    int flags;
    int status;
    int i;
    struct pollfd waits[2];
    va_list values;

    TEST_CHECK(argc >= 0 && argc < 15);
    va_start(values, argc);
    for (i = 0; i < argc; ++i) {
        arguments[i + 1] = va_arg(values, char *);
    }
    va_end(values);
    arguments[0] = (char *)executable;
    arguments[argc + 1] = NULL;
    TEST_CHECK(pipe(stdout_pipe) == 0);
    TEST_CHECK(pipe(stderr_pipe) == 0);
    child = fork();
    TEST_CHECK(child >= 0);
    if (child == 0) {
        if (dup2(stdout_pipe[1], STDOUT_FILENO) < 0 || dup2(stderr_pipe[1], STDERR_FILENO) < 0) {
            _exit(126);
        }
        (void)close(stdout_pipe[0]);
        (void)close(stdout_pipe[1]);
        (void)close(stderr_pipe[0]);
        (void)close(stderr_pipe[1]);
        execv(executable, arguments);
        _exit(127);
    }
    TEST_CHECK(close(stdout_pipe[1]) == 0);
    TEST_CHECK(close(stderr_pipe[1]) == 0);
    flags = fcntl(stdout_pipe[0], F_GETFL, 0);
    TEST_CHECK(flags >= 0 && fcntl(stdout_pipe[0], F_SETFL, flags | O_NONBLOCK) == 0);
    flags = fcntl(stderr_pipe[0], F_GETFL, 0);
    TEST_CHECK(flags >= 0 && fcntl(stderr_pipe[0], F_SETFL, flags | O_NONBLOCK) == 0);
    while (stdout_pipe[0] >= 0 || stderr_pipe[0] >= 0) {
        nfds_t count_fds = 0u;

        if (stdout_pipe[0] >= 0) {
            waits[count_fds].fd = stdout_pipe[0];
            waits[count_fds].events = POLLIN | POLLHUP;
            ++count_fds;
        }
        if (stderr_pipe[0] >= 0) {
            waits[count_fds].fd = stderr_pipe[0];
            waits[count_fds].events = POLLIN | POLLHUP;
            ++count_fds;
        }
        TEST_CHECK(poll(waits, count_fds, -1) >= 0);
        if (stdout_pipe[0] >= 0) {
            count = read(stdout_pipe[0], result->output + total_stdout,
                         sizeof(result->output) - total_stdout - 1u);
            if (count > 0) {
                total_stdout += (size_t)count;
                TEST_CHECK(total_stdout + 1u < sizeof(result->output));
            } else if (count == 0) {
                TEST_CHECK(close(stdout_pipe[0]) == 0);
                stdout_pipe[0] = -1;
            } else {
                TEST_CHECK(errno == EAGAIN || errno == EWOULDBLOCK);
            }
        }
        if (stderr_pipe[0] >= 0) {
            count = read(stderr_pipe[0], result->error + total_stderr,
                         sizeof(result->error) - total_stderr - 1u);
            if (count > 0) {
                total_stderr += (size_t)count;
                TEST_CHECK(total_stderr + 1u < sizeof(result->error));
            } else if (count == 0) {
                TEST_CHECK(close(stderr_pipe[0]) == 0);
                stderr_pipe[0] = -1;
            } else {
                TEST_CHECK(errno == EAGAIN || errno == EWOULDBLOCK);
            }
        }
    }
    result->output[total_stdout] = '\0';
    result->error[total_stderr] = '\0';
    TEST_CHECK(waitpid(child, &status, 0) == child);
    TEST_CHECK(WIFEXITED(status));
    result->exit_code = WEXITSTATUS(status);
}
#endif

static void test_argument_parsing(const char *executable)
{
    static const char *commands[] = {"list", "info", "spe-info", "spe-dump"};
    struct result result;
    size_t i;

    run_ost(executable, &result, 1, "--help");
    TEST_CHECK(result.exit_code == 0);
    TEST_CHECK(strstr(result.output, "list") != NULL);
    TEST_CHECK(strstr(result.output, "spe-dump") != NULL);
    run_ost(executable, &result, 0);
    TEST_CHECK(result.exit_code != 0);
    TEST_CHECK(strstr(result.error, "Usage:") != NULL);
    run_ost(executable, &result, 1, "not-a-command");
    TEST_CHECK(result.exit_code != 0);
    TEST_CHECK(strstr(result.error, "unknown command") != NULL);
    for (i = 0u; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        run_ost(executable, &result, 2, commands[i], "--help");
        TEST_CHECK(result.exit_code == 0);
        TEST_CHECK(strstr(result.output, "--json") != NULL);
        TEST_CHECK(strstr(result.output, commands[i]) != NULL);
    }
    run_ost(executable, &result, 2, "spe-info", "--json");
    TEST_CHECK(result.exit_code != 0);
    TEST_CHECK(strstr(result.error, "FILE") != NULL);
    run_ost(executable, &result, 5, "spe-dump", "--frame", "0", "one.spe", "two.spe");
    TEST_CHECK(result.exit_code != 0);
    TEST_CHECK(strstr(result.error, "Usage:") != NULL);
    run_ost(executable, &result, 4, "spe-dump", "--frame", "-1", "input.spe");
    TEST_CHECK(result.exit_code != 0);
    TEST_CHECK(strstr(result.error, "frame") != NULL);
}

static void test_spe_commands(const char *executable, const char *directory)
{
    char path[512];
    const char *first_csv = "index,value\n0,1\n1,2\n";
    const char *second_csv = "index,value\n0,300\n1,65535\n";
    struct result result;

    TEST_CHECK(snprintf(path, sizeof(path), "%s/fixture.spe", directory) > 0);
    write_fixture(path);
    run_ost(executable, &result, 2, "spe-info", path);
    TEST_CHECK(result.exit_code == 0);
    TEST_CHECK(strstr(result.output, "major version: 2") != NULL);
    TEST_CHECK(strstr(result.output, "frames: 2") != NULL);
    TEST_CHECK(strstr(result.output, "pixels per frame: 2") != NULL);
    run_ost(executable, &result, 3, "spe-info", "--json", path);
    TEST_CHECK(result.exit_code == 0);
    TEST_CHECK(strstr(result.output, "\"major_version\":2") != NULL);
    TEST_CHECK(strstr(result.output, "\"header_version\":2.5") != NULL);
    TEST_CHECK(strstr(result.output, "\"frame_count\":2") != NULL);
    TEST_CHECK(strstr(result.output, "\"data_type\":\"uint16\"") != NULL);
    TEST_CHECK(strstr(result.output, "\"software_version\":\"test \\\"reader\\\"\"") != NULL);
    run_ost(executable, &result, 2, "spe-dump", path);
    TEST_CHECK(result.exit_code == 0);
    TEST_CHECK(strcmp(result.output, first_csv) == 0);
    run_ost(executable, &result, 4, "spe-dump", "--frame", "1", path);
    TEST_CHECK(result.exit_code == 0);
    TEST_CHECK(strcmp(result.output, second_csv) == 0);
    run_ost(executable, &result, 4, "spe-dump", path, "--frame", "1");
    TEST_CHECK(result.exit_code == 0);
    TEST_CHECK(strcmp(result.output, second_csv) == 0);
    run_ost(executable, &result, 5, "spe-dump", "--json", "--frame", "1", path);
    TEST_CHECK(result.exit_code == 0);
    TEST_CHECK(strstr(result.output, "\"frame\":1") != NULL);
    TEST_CHECK(strstr(result.output, "\"pixels\":[300,65535]") != NULL);
    run_ost(executable, &result, 4, "spe-dump", "--frame", "2", path);
    TEST_CHECK(result.exit_code != 0);
    TEST_CHECK(strstr(result.error, "out of range") != NULL);
    run_ost(executable, &result, 2, "spe-info", "does-not-exist.spe");
    TEST_CHECK(result.exit_code != 0);
    TEST_CHECK(strstr(result.error, "I/O error") != NULL);
}

static void test_hardware_commands(const char *executable)
{
    struct result result;

    run_ost(executable, &result, 2, "list", "--json");
    TEST_CHECK(result.exit_code == 0);
    TEST_CHECK(result.output[0] == '[' || result.output[0] == '{');
    TEST_CHECK(strstr(result.output, "\"vendor_id\"") == NULL ||
               strstr(result.output, "\"product_id\"") != NULL);
    run_ost(executable, &result, 1, "list");
    TEST_CHECK(result.exit_code == 0);
}

int main(int argc, char **argv)
{
    char directory[] = "/tmp/ost-cli-test-XXXXXX";

    TEST_CHECK(argc == 2);
    TEST_CHECK(test_mkdtemp(directory) != NULL);
    test_argument_parsing(argv[1]);
    test_spe_commands(argv[1], directory);
    test_hardware_commands(argv[1]);
    return 0;
}
