/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#ifndef OPENST133_CAMERA_RECOVER_H
#define OPENST133_CAMERA_RECOVER_H

#include "openst133.h"

#include <stddef.h>

#define OST_CAMERA_STATUS_ACQUIRING 0xa5u

/* Recover the observed interrupted-acquisition status and retry command 0x42 once; other observed values pass through. */
int ost_camera_recover(
    ost_device *device,
    int read_first,
    unsigned char observed,
    unsigned char *status,
    char *error,
    size_t error_size);

#endif
