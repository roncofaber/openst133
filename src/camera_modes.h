/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#ifndef OPENST133_CAMERA_MODES_H
#define OPENST133_CAMERA_MODES_H

#include "openst133.h"

#include <stddef.h>
#include <stdint.h>

#define OST_CAMERA_READOUT_RATE_1_MHZ 1000000u
#define OST_CAMERA_READOUT_RATE_100_KHZ 100000u

typedef struct ost_camera_mode {
    size_t frame_bytes;
    size_t scan_table_slot;
    uint16_t readout_44;
    uint16_t readout_50;
    int rate_table;
} ost_camera_mode;

/* Slots 4 through 7 contain the supported mode tables that extend the four-table startup data file. */
int ost_camera_modes_table_info(size_t slot, uint16_t *command, size_t *count);
const unsigned char *ost_camera_modes_tables_sha256(void);

#endif
