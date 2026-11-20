/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#ifndef OPENST133_CAMERA_ACQUIRE_H
#define OPENST133_CAMERA_ACQUIRE_H

#include "camera_settings.h"
#include "camera_startup.h"
#include "openst133.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Single-frame acquisition sends two configuration blocks, transfer-size
 * setup, bounded 0xe2 polls and start, then reads the image on endpoint 0x82.
 * The settings plan selects the geometry, speed, exposure, shutter and gain.
 */

#define OST_ACQUIRE_FRAME_BYTES 268000u
#define OST_ACQUIRE_IMAGE_READS 17u

typedef struct ost_acquire_options {
    /* As ost_startup_options.read_first. */
    int read_first;
    unsigned int exposure_ms;
    /* Startup data file: the 0x20 and optional 0x48 tables of the configuration blocks are slots of it. */
    const ost_startup_data *data;
    /*
     * Required settings plan (camera_settings.h): readout, rate, exposure, shutter and gain word. With
     * plan->accumulations above 1 the camera returns that many frames, register 0x3c is set to the count and frame
     * receives plan->output_bytes of little-endian int32 sums.
     */
    const ost_settings_plan *plan;
    /* Register 0x3c selects one frame, ten frames or continuous streaming when this override is set. */
    int frame_count_set;
    uint16_t frame_count;
    /* Checked between image reads; when set the stop exchange is sent and OST_ERR_ABORTED returned. */
    atomic_int *abort_requested;
} ost_acquire_options;

/*
 * Run one acquisition into frame. Its required capacity is the selected mode's
 * frame size, which is OST_ACQUIRE_FRAME_BYTES for the default full unbinned
 * frame. received gets the bytes stored. Returns OST_SUCCESS,
 * OST_ERR_UNSUPPORTED (an unsupported setting or missing tables),
 * OST_ERR_BUFFER_TOO_SMALL, OST_ERR_ABORTED or OST_ERR_IO (a transfer failed or
 * an image read came back short; the frame is never padded). The two OP_POLL
 * phases use fixed bounds rather than readiness gates: exhausting a bound is
 * not an error. The exposure-dependent wait is the image transfer timeout.
 */
int ost_acquire_run(
    ost_device *device,
    const ost_acquire_options *options,
    unsigned char *frame,
    size_t capacity,
    size_t *received,
    char *error,
    size_t error_size);

#endif
