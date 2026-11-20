/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#ifndef OPENST133_CAMERA_SETTINGS_H
#define OPENST133_CAMERA_SETTINGS_H

#include "camera_scan.h"
#include "openst133.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Parameter-driven exposure setup. A combination is accepted only when every
 * command field is defined for it; otherwise the call returns
 * OST_ERR_UNSUPPORTED and names the unsupported field. Command 0x20 arguments
 * are generated from readout geometry. Other table bytes come from slots in
 * the startup data file supplied by the user.
 */

/* Supported exposure scales end at scale 8 and 60000 ms. */
#define OST_SETTINGS_MAX_EXPOSURE_SCALE 8u
#define OST_SETTINGS_MAX_EXPOSURE_MS 60000u

/* Controller gain words of command 0x4a for gains 1, 2 and 3. */
#define OST_SETTINGS_GAIN_WORD_1 0x0100u
#define OST_SETTINGS_GAIN_WORD_2 0x0140u
#define OST_SETTINGS_GAIN_WORD_3 0x0180u

/* Speed table currently held by the controller: the startup loads the 1 MHz table. */
typedef enum ost_settings_table {
    OST_SETTINGS_TABLE_1_MHZ = 0,
    OST_SETTINGS_TABLE_100_KHZ = 1,
    /* After a failed acquisition that may have sent the 0x48 record. */
    OST_SETTINGS_TABLE_UNKNOWN = 2
} ost_settings_table;

typedef struct ost_settings {
    /* Zero selects the camera's bias exposure. */
    unsigned int exposure_ms;
    /* Disabled Closed (0x3a = 4) instead of Normal (0x3a = 6). */
    int shutter_closed;
    /* 0 selects 1 MHz. */
    uint32_t readout_rate_hz;
    /* NULL selects the full 1340 x 100 frame without binning. */
    const ost_camera_readout *readout;
    /* Disabled Opened (0x3a = 0x86); INVALID_ARGUMENT together with shutter_closed. */
    int shutter_opened;
    /* Pre Open requires External Sync timing (0x3a = 0x1e), which is unsupported. */
    int pre_open;
    /* Controller gain 1, 2 or 3 (the 16-bit gain word of command 0x4a); 0 selects the stored gain 2. */
    unsigned int gain;
    /* Frames the camera reads per acquisition (register 0x3c), summed by the host; 0 selects 1. */
    unsigned int accumulations;
} ost_settings;

typedef struct ost_settings_plan {
    unsigned int exposure_count;
    unsigned int exposure_scale;
    uint16_t reg_3a;
    uint16_t reg_42;
    uint16_t reg_44;
    uint16_t reg_50;
    uint16_t gain_word;
    /* Register 0x3c value: the number of frames the camera returns and the host sums. */
    uint16_t accumulations;
    /* Bytes of one camera frame (16-bit samples). */
    size_t frame_bytes;
    /* Bytes the caller receives: frame_bytes for one frame, 32-bit sums otherwise. */
    size_t output_bytes;
    uint16_t size_low;
    uint16_t size_high;
    /* Slot of the startup data file that holds the first 0x20 scan program (1 is the full frame). */
    size_t scan_table_slot;
    ost_camera_scan_program scan;
    /* Send 0x40 = 0x96, 0x40 = 0x97 and the slow 0x48 record in the first configuration block. */
    int insert_rate_table;
    ost_settings_table table_after;
} ost_settings_plan;

/* Normalized 0x46 code of a target in Celsius; INVALID_ARGUMENT outside -25 to -152 C. */
int ost_settings_setpoint_code(double celsius, int *code, char *error, size_t error_size);

/*
 * Select the smallest scale whose real quotient exposure_ms / 2^scale is at most 255, then truncate that quotient
 * to count. Zero gives count 0 and scale 0.
 * UNSUPPORTED above OST_SETTINGS_MAX_EXPOSURE_MS; count * 2^scale can be less than exposure_ms.
 */
int ost_settings_encode_exposure(unsigned int exposure_ms, unsigned int *count, unsigned int *scale,
    char *error, size_t error_size);

/*
 * Resolve settings against the table the controller currently holds. OST_ERR_INVALID_ARGUMENT for an ROI outside
 * the 1340 x 100 sensor or with partial bins, OST_ERR_UNSUPPORTED (with the undetermined field in error) for a
 * valid request the protocol rules do not determine.
 */
int ost_settings_plan_make(const ost_settings *settings, ost_settings_table loaded, ost_settings_plan *plan,
    char *error, size_t error_size);

#endif
