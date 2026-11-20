/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#ifndef OPENST133_CAMERA_STARTUP_H
#define OPENST133_CAMERA_STARTUP_H

#include "openst133.h"

#include <stddef.h>
#include <stdint.h>

/* Startup, identification, temperature reads and setpoint writes for ST-133. */

#define OST_STARTUP_POLL_LOOPS 3u
#define OST_STARTUP_POLL_DEFAULT_MIN_MS 100u
#define OST_STARTUP_NAME_SIZE 32u

/* Constants of the supported camera configuration. */
#define OST_STARTUP_CONTROLLER_TYPE 10u
#define OST_STARTUP_DETECTOR_ID_EEV_100X1340B 98u
#define OST_STARTUP_STATUS_LIVE 0xa7u
#define OST_STARTUP_SCALE_CELSIUS_PER_COUNT 0.5
#define OST_STARTUP_RANGE_MAX_CELSIUS (-25.0)
#define OST_STARTUP_CALIBRATION_CELSIUS 0.0
#define OST_STARTUP_SETPOINT_RANGE_COUNTS 254u
#define OST_STARTUP_READS_PER_TEMPERATURE 11u
#define OST_STARTUP_DEFAULT_TARGET_CELSIUS (-120.0)

/*
 * Write-block and mode tables (0x48 and 0x20 records). Build the data file from
 * your licensed WinSpec installation with openst133 startup-data.
 * The library loads and verifies the file; do not redistribute its contents.
 */
typedef struct ost_startup_data ost_startup_data;

/* Path: OPENST133_STARTUP_DATA, else OPENST133_DATA/firmware/st133-startup.bin, then the platform default.
 * Windows uses APPDATA/openst133/firmware/st133-startup.bin, or ./openst133/firmware/st133-startup.bin without APPDATA.
 * Linux uses HOME/.local/share/openst133/firmware/st133-startup.bin, with . as the base when HOME is absent. */
const char *ost_startup_data_path(char *buffer, size_t size);

/*
 * Load and verify the data file (NULL path selects ost_startup_data_path). A missing or mismatched file returns
 * OST_ERR_UNSUPPORTED with error text that names the startup-data command and user guide.
 */
int ost_startup_data_load(const char *path, ost_startup_data **data, char *error, size_t error_size);
void ost_startup_data_free(ost_startup_data *data);

/* One verified table by startup-order slot; NULL when the slot does not exist. */
const unsigned char *ost_startup_data_table(const ost_startup_data *data, size_t slot, uint16_t *command, size_t *count);

typedef struct ost_startup_options {
    /* Read before requesting on replay; USB hardware requests first and then reads. */
    int read_first;
    /* Number of 0xa2 requests in each time-bounded wait; 0 selects the default counts. */
    unsigned int poll_counts[OST_STARTUP_POLL_LOOPS];
    /* Minimum duration of each wait, padded with an idle sleep; the default is approximately 100 ms. */
    unsigned int poll_min_ms;
    /* Required: the write blocks are tables of this file. */
    const ost_startup_data *data;
    /* Startup setpoint code; -1 selects the metadata default or the -120 C startup default. */
    int setpoint_code;
} ost_startup_options;

typedef struct ost_startup_identity {
    uint32_t controller_type;
    uint32_t detector_id;
    char detector_name[OST_STARTUP_NAME_SIZE];
    uint32_t width;
    uint32_t height;
    int target_known;
    int target_celsius;
    int scale_record_found;
} ost_startup_identity;

void ost_startup_options_default(ost_startup_options *options);

/*
 * Run the startup sequence on an opened device: identification, then the five setup write blocks. error receives
 * text on failure. Returns OST_SUCCESS, OST_ERR_IO (a transfer failed or a response differs from its expected
 * value), OST_ERR_UNSUPPORTED (the camera is not the supported configuration: the identity reads lack the ZZ
 * signature, report a controller type other than 10 or a detector other than 98, EEV 100x1340B; the error starts
 * with "camera not recognized: " and nothing has been written) or OST_ERR_INVALID_ARGUMENT.
 */
int ost_startup_run(
    ost_device *device,
    const ost_startup_options *options,
    ost_startup_identity *identity,
    char *error,
    size_t error_size);

/* Decode a supported detector identity: 0 on success, -1 for an unknown ID. */
int ost_startup_decode_detector(uint32_t detector_id, char *name, size_t name_size, uint32_t *width, uint32_t *height);

/* Pure temperature and setpoint conversions. */
double ost_startup_temperature_from_adc(double mean_adc);
double ost_startup_round_temperature(double celsius);
/* Normalized setpoint code; returns -1 when the target is outside the selected range or not finite. */
int ost_startup_setpoint_code(double celsius);

/* Temperature and setpoint exchange groups on an identified device. */
int ost_startup_read_temperature(
    ost_device *device,
    const ost_startup_identity *identity,
    int read_first,
    double *celsius,
    char *error,
    size_t error_size);
/* Write any normalized setpoint code in the selected range. */
int ost_startup_write_setpoint(
    ost_device *device,
    const ost_startup_identity *identity,
    int read_first,
    double celsius,
    char *error,
    size_t error_size);

#endif
