/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

/*
 * ost acquire geometry flags: --fvb composes with --roi and with the
 * horizontal factor of --bin, and a conflicting vertical factor is refused before any camera traffic.
 */

#define _POSIX_C_SOURCE 200809L

#include "openst133.h"
#include "camera_settings.h"
#include "test_check.h"

#include <fcntl.h>
#include <stdint.h>

#define main ost_cli_main
#include "../src/cli/ost.c"
#undef main

int ost_temperature_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return EXIT_FAILURE;
}

static int sim_list(void *userdata, ost_device_info **devices, size_t *count)
{
    (void)userdata;
    *devices = NULL;
    *count = 0u;
    return OST_SUCCESS;
}

static void sim_free(void *userdata, ost_device_info *devices)
{
    (void)userdata;
    free(devices);
}

static int sim_open(void *userdata, const ost_device_info *info, ost_transport_handle **handle)
{
    (void)info;
    *handle = (ost_transport_handle *)userdata;
    return OST_SUCCESS;
}

static void sim_close(void *userdata, ost_transport_handle *handle)
{
    (void)userdata;
    (void)handle;
}

static int sim_control(void *userdata, ost_transport_handle *handle, uint8_t request_type, uint8_t request,
    uint16_t value, uint16_t index, unsigned char *data, uint16_t length, unsigned int timeout_ms, int *transferred)
{
    unsigned int *transfers = userdata;

    (void)handle;
    (void)request_type;
    (void)request;
    (void)value;
    (void)index;
    (void)data;
    (void)timeout_ms;
    ++*transfers;
    *transferred = length;
    return OST_SUCCESS;
}

static int sim_bulk(void *userdata, ost_transport_handle *handle, unsigned char endpoint, unsigned char *data,
    int length, unsigned int timeout_ms, int *transferred)
{
    unsigned int *transfers = userdata;

    (void)handle;
    (void)endpoint;
    (void)data;
    (void)timeout_ms;
    ++*transfers;
    *transferred = length;
    return OST_SUCCESS;
}

typedef struct geometry_case {
    const char *name;
    acquire_geometry_flags flags;
    ost_camera_readout expected;
    uint32_t columns;
    uint32_t rows;
} geometry_case;

static const geometry_case composed[] = {
    {"--fvb", {1, 0, {0u, 0u, 0u, 0u}, 0, {1u, 1u}}, {0u, 0u, 1340u, 100u, 1u, 100u}, 1340u, 1u},
    {"--fvb --bin 2x1", {1, 0, {0u, 0u, 0u, 0u}, 1, {2u, 1u}}, {0u, 0u, 1340u, 100u, 2u, 100u}, 670u, 1u},
    {"--fvb --bin 2x100", {1, 0, {0u, 0u, 0u, 0u}, 1, {2u, 100u}}, {0u, 0u, 1340u, 100u, 2u, 100u}, 670u, 1u},
    {"--fvb --roi 200,0,800,100", {1, 1, {200u, 0u, 800u, 100u}, 0, {1u, 1u}}, {200u, 0u, 800u, 100u, 1u, 100u},
        800u, 1u},
    {"--fvb --roi 0,40,400,20 --bin 4x1", {1, 1, {0u, 40u, 400u, 20u}, 1, {4u, 1u}}, {0u, 40u, 400u, 20u, 4u, 20u},
        100u, 1u},
    {"--fvb --roi 0,40,400,20 --bin 4x20", {1, 1, {0u, 40u, 400u, 20u}, 1, {4u, 20u}},
        {0u, 40u, 400u, 20u, 4u, 20u}, 100u, 1u},
    {"--roi 0,40,400,20 --bin 4x2", {0, 1, {0u, 40u, 400u, 20u}, 1, {4u, 2u}}, {0u, 40u, 400u, 20u, 4u, 2u},
        100u, 10u},
    {"--bin 2x1", {0, 0, {0u, 0u, 0u, 0u}, 1, {2u, 1u}}, {0u, 0u, 1340u, 100u, 2u, 1u}, 670u, 100u},
};

static const geometry_case refused[] = {
    {"--fvb --bin 2x5", {1, 0, {0u, 0u, 0u, 0u}, 1, {2u, 5u}}, {0u, 0u, 0u, 0u, 0u, 0u}, 0u, 0u},
    {"--fvb --roi 0,40,400,20 --bin 4x100", {1, 1, {0u, 40u, 400u, 20u}, 1, {4u, 100u}},
        {0u, 0u, 0u, 0u, 0u, 0u}, 0u, 0u},
};

static int same_readout(const ost_camera_readout *a, const ost_camera_readout *b)
{
    return a->x == b->x && a->y == b->y && a->width == b->width && a->height == b->height &&
        a->x_binning == b->x_binning && a->y_binning == b->y_binning;
}

static void print_readout(const char *label, const ost_camera_readout *r)
{
    (void)fprintf(stderr, "  %s: x=%u y=%u w=%u h=%u bin=%ux%u\n", label, r->x, r->y, r->width, r->height,
        r->x_binning, r->y_binning);
}

/* Each combination on a simulated camera: the readout the camera ends up with and the frame shape it gives. */
static void test_simulated_camera(void)
{
    const ost_camera_readout full = {0u, 0u, 1340u, 100u, 1u, 1u};
    ost_device_info info = {OST_USB_VENDOR_ID, OST_USB_PRODUCT_ID, 1u, 5u, 0u, {0u}};
    unsigned int transfers = 0u;
    ost_transport transport;
    ost_camera *camera = NULL;
    ost_camera_readout readout;
    char message[320];
    size_t i;

    memset(&transport, 0, sizeof(transport));
    transport.userdata = &transfers;
    transport.list_devices = sim_list;
    transport.free_devices = sim_free;
    transport.open = sim_open;
    transport.close = sim_close;
    transport.control_transfer = sim_control;
    transport.bulk_transfer = sim_bulk;
    TEST_CHECK(ost_camera_open(&transport, &info, &camera) == OST_SUCCESS);

    for (i = 0u; i < sizeof(composed) / sizeof(composed[0]); ++i) {
        const geometry_case *c = &composed[i];

        TEST_CHECK(ost_camera_set_readout(camera, &full) == OST_SUCCESS);
        if (acquire_apply_geometry(camera, &c->flags, message, sizeof(message)) != OST_SUCCESS) {
            (void)fprintf(stderr, "%s refused: %s / %s\n", c->name, message, ost_camera_last_error(camera));
            TEST_CHECK(0);
        }
        TEST_CHECK(message[0] == '\0');
        TEST_CHECK(ost_camera_get_readout(camera, &readout) == OST_SUCCESS);
        if (!same_readout(&readout, &c->expected)) {
            (void)fprintf(stderr, "%s composed the wrong readout\n", c->name);
            print_readout("got", &readout);
            print_readout("expected", &c->expected);
            TEST_CHECK(0);
        }
        TEST_CHECK(readout.width / readout.x_binning == c->columns);
        TEST_CHECK(readout.height / readout.y_binning == c->rows);
    }

    for (i = 0u; i < sizeof(refused) / sizeof(refused[0]); ++i) {
        const geometry_case *c = &refused[i];

        TEST_CHECK(ost_camera_set_readout(camera, &full) == OST_SUCCESS);
        TEST_CHECK(acquire_apply_geometry(camera, &c->flags, message, sizeof(message)) == OST_ERR_INVALID_ARGUMENT);
        if (strstr(message, "--fvb bins all") == NULL || strstr(message, "x1 (the same as --bin") == NULL) {
            (void)fprintf(stderr, "%s: unclear refusal \"%s\"\n", c->name, message);
            TEST_CHECK(0);
        }
        TEST_CHECK(ost_camera_get_readout(camera, &readout) == OST_SUCCESS);
        TEST_CHECK(same_readout(&readout, &full));
    }
    TEST_CHECK(strstr(message, "all 20 rows") != NULL && strstr(message, "--bin 4x1") != NULL);

    ost_camera_close(camera);
}

/* The settings planner accepts each composed FVB geometry and sizes its spectrum. */
static void test_generated_plan(void)
{
    static const size_t frame_bytes[] = {2680u, 1340u, 1340u, 1600u, 200u, 200u};
    char error[256];
    size_t i;

    for (i = 0u; i < sizeof(frame_bytes) / sizeof(frame_bytes[0]); ++i) {
        ost_settings settings;
        ost_settings_plan plan;
        int status;

        memset(&settings, 0, sizeof(settings));
        settings.exposure_ms = 100u;
        settings.readout = &composed[i].expected;
        status = ost_settings_plan_make(&settings, OST_SETTINGS_TABLE_1_MHZ, &plan, error, sizeof(error));
        if (status != OST_SUCCESS || plan.frame_bytes != frame_bytes[i]) {
            (void)fprintf(stderr, "%s: plan status %d, %zu bytes (expected %zu): %s\n", composed[i].name, status,
                status == OST_SUCCESS ? plan.frame_bytes : 0u, frame_bytes[i], status == OST_SUCCESS ? "" : error);
            TEST_CHECK(0);
        }
    }
}

static int run_cli(char **argv, int argc, char *output, size_t capacity, const char *capture_path)
{
    int saved;
    int fd;
    int result;
    FILE *file;
    size_t used;

    (void)fflush(stderr);
    saved = dup(STDERR_FILENO);
    TEST_CHECK(saved >= 0);
    fd = open(capture_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    TEST_CHECK(fd >= 0);
    TEST_CHECK(dup2(fd, STDERR_FILENO) >= 0);
    (void)close(fd);
    result = ost_cli_main(argc, argv);
    (void)fflush(stderr);
    TEST_CHECK(dup2(saved, STDERR_FILENO) >= 0);
    (void)close(saved);
    file = fopen(capture_path, "rb");
    TEST_CHECK(file != NULL);
    used = fread(output, 1u, capacity - 1u, file);
    output[used] = '\0';
    (void)fclose(file);
    TEST_CHECK(unlink(capture_path) == 0);
    return result;
}

/*
 * Through the CLI entry point: the conflicting combination is refused with the explanation before any device is
 * listed, and the composing combination passes the flag check.
 */
static void test_cli_entry(const char *directory)
{
    char out[1100];
    char capture[1100];
    char text[2048];
    char *conflict[] = {"ost", "acquire", "--exposure", "0.1", "--out", out, "--fvb",
        "--bin", "2x5", NULL};
    char *compose[] = {"ost", "acquire", "--exposure", "0.1", "--out", out, "--fvb",
        "--bin", "2x1", NULL};
    FILE *file;

    (void)snprintf(out, sizeof(out), "%s/spectrum.spe", directory);
    (void)snprintf(capture, sizeof(capture), "%s/stderr.txt", directory);
    TEST_CHECK(test_setenv("OPENST133_NO_HARDWARE", "1", 1) == 0);

    TEST_CHECK(run_cli(conflict, 9, text, sizeof(text), capture) == EXIT_FAILURE);
    if (strstr(text, "ost: acquire: --fvb bins all 100 rows of the region into one spectrum, so --bin 2x5 conflicts")
            == NULL || strstr(text, "use --bin 2x1 (the same as --bin 2x100)") == NULL ||
        strstr(text, "OPENST133_NO_HARDWARE") != NULL) {
        (void)fprintf(stderr, "conflicting flags gave:\n%s", text);
        TEST_CHECK(0);
    }
    file = fopen(out, "rb");
    TEST_CHECK(file == NULL);

    TEST_CHECK(run_cli(compose, 9, text, sizeof(text), capture) == EXIT_FAILURE);
    /* Past the flag check, ost stops at the hardware guard, or earlier when no camera is attached to list. */
    if (strstr(text, "--fvb bins all") != NULL ||
        (strstr(text, "OPENST133_NO_HARDWARE") == NULL && strstr(text, "no ST-133 found") == NULL)) {
        (void)fprintf(stderr, "composing flags gave:\n%s", text);
        TEST_CHECK(0);
    }
    file = fopen(out, "rb");
    TEST_CHECK(file == NULL);
}

int main(void)
{
    char directory[1024];
    const char *tmp = getenv("TMPDIR");

    test_simulated_camera();
    test_generated_plan();
    (void)snprintf(directory, sizeof(directory), "%s/ost-cli-geometry-XXXXXX",
        tmp != NULL && tmp[0] != '\0' ? tmp : "/tmp");
    TEST_CHECK(test_mkdtemp(directory) != NULL);
    test_cli_entry(directory);
    TEST_CHECK(rmdir(directory) == 0);
    (void)printf("--fvb composes with --roi and --bin Nx1 or Nx<rows>; other vertical factors are refused\n");
    return EXIT_SUCCESS;
}
