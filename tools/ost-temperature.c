/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#include "openst133.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ost_temperature_main(int argc, char **argv);
int ost_cli_choose_device(
    const char *command, const ost_device_info *devices, size_t count, const char *selector, size_t *index);

static void usage(FILE *stream)
{
    fputs("Usage: ost temperature "
          "[--open-target CELSIUS] [--set-target CELSIUS] [--json] [--camera SELECTOR]\n", stream);
}

static void help(void)
{
    usage(stdout);
    puts("Open the ST-133, identify it and print its temperature. This sends camera commands, including the");
    puts("startup sequence and the saved temperature target, and needs the startup data from 'openst133 setup'.");
    puts("--open-target CELSIUS overrides the target written at open, from -152 to -25 C.");
    puts("--set-target CELSIUS writes and persists the setpoint after the temperature read.");
    puts("--json selects machine-readable output.");
    puts("--camera SELECTOR picks INDEX from 'ost list', a BUS-PORT[.PORT] path or BUS:ADDR; required with several cameras.");
}

static int parse_celsius(const char *text, double *value)
{
    char *end = NULL;

    if (text[0] == '\0') {
        return 0;
    }
    *value = strtod(text, &end);
    return end != text && *end == '\0' && isfinite(*value);
}

static const char *target_source_name(ost_camera_target_source source)
{
    switch (source) {
    case OST_CAMERA_TARGET_SOURCE_OPTION:
        return "option";
    case OST_CAMERA_TARGET_SOURCE_PERSISTED:
        return "persisted";
    case OST_CAMERA_TARGET_SOURCE_CAMERA_DEFAULT:
        return "camera default";
    case OST_CAMERA_TARGET_SOURCE_STARTUP_DEFAULT:
        return "startup default";
    default:
        return "none";
    }
}

int ost_temperature_main(int argc, char **argv)
{
    const char *camera_selector = NULL;
    int json = 0;
    int has_target = 0;
    double target = 0.0;
    ost_device_info *devices = NULL;
    size_t count = 0u;
    size_t device_index = 0u;
    ost_camera *camera = NULL;
    ost_camera_identity identity;
    ost_camera_open_options open_options;
    ost_camera_target_source target_source = OST_CAMERA_TARGET_SOURCE_NONE;
    double celsius = 0.0;
    int status;
    int i;

    ost_camera_open_options_default(&open_options);
    for (i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            help();
            return EXIT_SUCCESS;
        } else if (strcmp(argv[i], "--json") == 0) {
            json = 1;
        } else if (strcmp(argv[i], "--open-target") == 0) {
            if (++i == argc || !parse_celsius(argv[i], &open_options.target_celsius)) {
                fputs("ost: --open-target requires a temperature in Celsius\n", stderr);
                return EXIT_FAILURE;
            }
            open_options.target_set = 1;
        } else if (strcmp(argv[i], "--set-target") == 0) {
            if (++i == argc || !parse_celsius(argv[i], &target)) {
                fputs("ost: --set-target requires a temperature in Celsius\n", stderr);
                return EXIT_FAILURE;
            }
            has_target = 1;
        } else if (strcmp(argv[i], "--camera") == 0) {
            if (++i == argc) {
                fputs("ost: --camera requires an index, BUS-PORT path or BUS:ADDR\n", stderr);
                return EXIT_FAILURE;
            }
            camera_selector = argv[i];
        } else {
            fprintf(stderr, "ost: unknown option: %s\n", argv[i]);
            usage(stderr);
            return EXIT_FAILURE;
        }
    }
    status = ost_list_devices(NULL, &devices, &count);
    if (status != OST_SUCCESS) {
        ost_free_devices(devices);
        fprintf(stderr, "ost: temperature: %s\n", ost_status_text(status));
        return EXIT_FAILURE;
    }
    if (ost_cli_choose_device("temperature", devices, count, camera_selector, &device_index) != OST_SUCCESS) {
        ost_free_devices(devices);
        return EXIT_FAILURE;
    }
    status = ost_camera_open_with_options(NULL, &devices[device_index], &open_options, &camera);
    ost_free_devices(devices);
    if (status != OST_SUCCESS) {
        fprintf(stderr, "ost: temperature: open failed: %s (library error %d)%s%s\n", ost_status_text(status),
            status, ost_camera_open_error()[0] != '\0' ? ": " : "", ost_camera_open_error());
        return EXIT_FAILURE;
    }
    status = ost_camera_get_identity(camera, &identity);
    if (status == OST_SUCCESS) {
        status = ost_camera_get_target_source(camera, &target_source);
    }
    if (status == OST_SUCCESS) {
        status = ost_camera_get_temperature(camera, &celsius);
    }
    if (status == OST_SUCCESS && has_target) {
        status = ost_camera_set_temperature(camera, target);
    }
    if (status == OST_SUCCESS && has_target) {
        status = ost_camera_get_identity(camera, &identity);
    }
    if (status != OST_SUCCESS) {
        fprintf(stderr, "ost: temperature: %s (library error %d): %s\n", ost_status_text(status), status,
            ost_camera_last_error(camera));
        ost_camera_close(camera);
        return EXIT_FAILURE;
    }
    if (json) {
        printf("{\"detector\":\"%s\",\"detector_id\":%u,\"width\":%u,\"height\":%u,\"celsius\":%.2f,"
               "\"target_celsius\":%.2f,\"target_written\":%s,\"target_source\":\"%s\"}\n",
            identity.detector_name, (unsigned int)identity.detector_id, (unsigned int)identity.width,
            (unsigned int)identity.height, celsius, identity.target_celsius, has_target ? "true" : "false",
            target_source_name(target_source));
    } else {
        printf("camera: %s (detector %u), %ux%u\n", identity.detector_name, (unsigned int)identity.detector_id,
            (unsigned int)identity.width, (unsigned int)identity.height);
        printf("temperature: %.2f C\n", celsius);
        if (identity.target_known || has_target) {
            printf("target: %.2f C%s\n", identity.target_celsius, has_target ? " (written)" : " (stored)");
            printf("target source: %s\n", target_source_name(target_source));
        }
    }
    ost_camera_close(camera);
    return EXIT_SUCCESS;
}
