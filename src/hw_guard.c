/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#include "hw_guard.h"

#include "openst133.h"

#include <stdlib.h>
#include <string.h>

int ost_hw_guard_check(void)
{
    const char *value = getenv(OST_NO_HARDWARE_ENV);

    return value != NULL && strcmp(value, "1") == 0 ? OST_ERR_UNSUPPORTED : OST_SUCCESS;
}

const char *ost_hw_guard_error(void)
{
    return "OPENST133_NO_HARDWARE=1 forbids opening real hardware; unset it or set it to 0 only when real-device access is intentionally enabled";
}
