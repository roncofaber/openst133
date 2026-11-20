/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#include "camera_settings.h"

#include "camera_modes.h"
#include "camera_startup.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define SENSOR_WIDTH 1340u
#define SENSOR_HEIGHT 100u
#define SHUTTER_NORMAL 6u
#define SHUTTER_CLOSED 4u
#define SHUTTER_OPENED 0x86u

/* The supported readout modes use these scan-table slots in startup data. */
#define SLOT_FULL 1u
#define SLOT_FULL_VERTICAL_BINNING 4u
#define SLOT_ROI 5u
#define SLOT_HORIZONTAL_BINNING_2 6u

static void fail(char *error, size_t error_size, const char *format, ...)
{
    va_list arguments;

    if (error == NULL || error_size == 0u) {
        return;
    }
    va_start(arguments, format);
    (void)vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static const uint16_t k_gain_words[3] = {OST_SETTINGS_GAIN_WORD_1, OST_SETTINGS_GAIN_WORD_2, OST_SETTINGS_GAIN_WORD_3};

int ost_settings_setpoint_code(double celsius, int *code, char *error, size_t error_size)
{
    int value = ost_startup_setpoint_code(celsius);

    if (value < 0) {
        fail(error, error_size, "setpoint %g C is outside the selected range of -152 C to -25 C", celsius);
        return OST_ERR_INVALID_ARGUMENT;
    }
    if (code != NULL) {
        *code = value;
    }
    return OST_SUCCESS;
}

int ost_settings_encode_exposure(unsigned int exposure_ms, unsigned int *count, unsigned int *scale,
    char *error, size_t error_size)
{
    unsigned int s = 0u;

    if (exposure_ms > OST_SETTINGS_MAX_EXPOSURE_MS) {
        fail(error, error_size,
            "an exposure of %u ms exceeds the supported maximum of %u ms; the exposure count and command 0x34 "
            "scale above %u are unsupported", exposure_ms, OST_SETTINGS_MAX_EXPOSURE_MS,
            OST_SETTINGS_MAX_EXPOSURE_SCALE);
        return OST_ERR_UNSUPPORTED;
    }
    while (((exposure_ms + (1u << s) - 1u) >> s) > 255u) {
        ++s;
    }
    if (count != NULL) {
        *count = exposure_ms >> s;
    }
    if (scale != NULL) {
        *scale = s;
    }
    return OST_SUCCESS;
}

int ost_settings_plan_make(const ost_settings *settings, ost_settings_table loaded, ost_settings_plan *plan,
    char *error, size_t error_size)
{
    static const ost_camera_readout full = {0u, 0u, SENSOR_WIDTH, SENSOR_HEIGHT, 1u, 1u};
    const ost_camera_readout *readout;
    uint32_t rate;
    unsigned int count = 0u;
    unsigned int scale = 0u;
    int slow;
    int full_area;
    size_t slot = 0u;
    size_t columns;
    size_t rows;
    unsigned int gain;
    unsigned int accumulations;
    int status;

    if (settings == NULL || plan == NULL) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    gain = settings->gain;
    accumulations = settings->accumulations;
    readout = settings->readout != NULL ? settings->readout : &full;
    rate = settings->readout_rate_hz != 0u ? settings->readout_rate_hz : OST_CAMERA_READOUT_RATE_1_MHZ;
    memset(plan, 0, sizeof(*plan));
    if (readout->width == 0u || readout->height == 0u || readout->x_binning == 0u || readout->y_binning == 0u ||
        readout->x >= SENSOR_WIDTH || readout->y >= SENSOR_HEIGHT || readout->width > SENSOR_WIDTH - readout->x ||
        readout->height > SENSOR_HEIGHT - readout->y) {
        fail(error, error_size, "ROI and binning must have positive extents and bins and fit inside the 1340 x 100 sensor");
        return OST_ERR_INVALID_ARGUMENT;
    }
    if (settings->shutter_closed && settings->shutter_opened) {
        fail(error, error_size, "the shutter cannot be both Disabled Closed and Disabled Opened");
        return OST_ERR_INVALID_ARGUMENT;
    }
    if (settings->pre_open) {
        fail(error, error_size,
            "Pre Open (command 0x3a bit 0x08) requires External Sync timing (0x3a = 0x1e); triggered exposure "
            "and the exchange after its image request are unsupported");
        return OST_ERR_UNSUPPORTED;
    }
    if (gain > 3u) {
        fail(error, error_size,
            "controller gain %u has no command 0x4a gain word: only gains 1, 2 and 3 exist", gain);
        return OST_ERR_UNSUPPORTED;
    }
    if (gain == 0u) {
        gain = 2u;
    }
    if (accumulations == 0u) {
        accumulations = 1u;
    }
    if (accumulations != 1u && accumulations != 3u && accumulations != 10u) {
        fail(error, error_size,
            "%u accumulations has no supported register 0x3c encoding: choose 1, 3 or 10 frames per acquisition",
            accumulations);
        return OST_ERR_UNSUPPORTED;
    }
    status = ost_settings_encode_exposure(settings->exposure_ms, &count, &scale, error, error_size);
    if (status != OST_SUCCESS) {
        return status;
    }
    if (rate != OST_CAMERA_READOUT_RATE_1_MHZ && rate != OST_CAMERA_READOUT_RATE_100_KHZ) {
        fail(error, error_size,
            "readout rate %u Hz has no supported command 0x42/0x44/0x50 profile: choose 1000000 Hz or 100000 Hz",
            (unsigned int)rate);
        return OST_ERR_UNSUPPORTED;
    }
    slow = rate == OST_CAMERA_READOUT_RATE_100_KHZ;
    full_area = readout->x == 0u && readout->y == 0u && readout->width == SENSOR_WIDTH &&
        readout->height == SENSOR_HEIGHT;
    if (full_area && readout->x_binning == 1u && readout->y_binning == 1u) {
        slot = SLOT_FULL;
    } else if (full_area && readout->x_binning == 1u && readout->y_binning == SENSOR_HEIGHT) {
        slot = SLOT_FULL_VERTICAL_BINNING;
    } else if (readout->x == 100u && readout->y == 20u && readout->width == 200u && readout->height == 40u &&
        readout->x_binning == 1u && readout->y_binning == 1u) {
        slot = SLOT_ROI;
    } else if (full_area && readout->x_binning == 2u && readout->y_binning == 1u) {
        slot = SLOT_HORIZONTAL_BINNING_2;
    }
    status = ost_camera_scan_generate(readout, &plan->scan, error, error_size);
    if (status != OST_SUCCESS) {
        return status;
    }
    if (slow && !(full_area && readout->x_binning == 1u && readout->y_binning == 1u)) {
        fail(error, error_size,
            "command 0x48 at 100000 Hz supports only the full 1340 x 100 frame without binning");
        return OST_ERR_UNSUPPORTED;
    }
    if (loaded == OST_SETTINGS_TABLE_1_MHZ) {
        plan->insert_rate_table = slow;
        plan->table_after = slow ? OST_SETTINGS_TABLE_100_KHZ : OST_SETTINGS_TABLE_1_MHZ;
    } else {
        fail(error, error_size,
            "command 0x48 requires the 1 MHz startup speed table, but the controller holds the %s speed table; "
            "reopen the camera to reload the 1 MHz table",
            loaded == OST_SETTINGS_TABLE_100_KHZ ? "100 kHz" : "unknown");
        return OST_ERR_UNSUPPORTED;
    }
    columns = plan->scan.columns;
    rows = plan->scan.rows;
    plan->exposure_count = count;
    plan->exposure_scale = scale;
    plan->reg_3a = settings->shutter_closed ? SHUTTER_CLOSED : settings->shutter_opened ? SHUTTER_OPENED : SHUTTER_NORMAL;
    plan->gain_word = (uint16_t)k_gain_words[gain - 1u];
    plan->accumulations = (uint16_t)accumulations;
    plan->reg_42 = 0x36u;
    plan->reg_44 = slow ? 0x33u : 0x03u;
    plan->reg_50 = slow ? 0x00u : 0x80u;
    plan->frame_bytes = columns * rows * OST_CAMERA_BYTES_PER_PIXEL;
    plan->size_low = (uint16_t)(plan->frame_bytes & 0xffffu);
    plan->size_high = (uint16_t)((plan->frame_bytes >> 16u) & 0xffffu);
    plan->output_bytes = accumulations == 1u ? plan->frame_bytes : columns * rows * sizeof(int32_t);
    plan->scan_table_slot = slot;
    return OST_SUCCESS;
}
