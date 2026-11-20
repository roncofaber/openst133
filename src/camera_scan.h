/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#ifndef OPENST133_CAMERA_SCAN_H
#define OPENST133_CAMERA_SCAN_H

#include "openst133.h"

#include <stddef.h>
#include <stdint.h>

#define OST_CAMERA_SCAN_MAX_ARGUMENTS 8192u

typedef struct ost_camera_scan_program {
    size_t count;
    uint32_t columns;
    uint32_t rows;
    unsigned char arguments[OST_CAMERA_SCAN_MAX_ARGUMENTS];
} ost_camera_scan_program;

int ost_camera_scan_generate(const ost_camera_readout *readout, ost_camera_scan_program *program,
    char *error, size_t error_size);

#endif
