/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#ifndef OPENST133_HW_GUARD_H
#define OPENST133_HW_GUARD_H

#define OST_NO_HARDWARE_ENV "OPENST133_NO_HARDWARE"

int ost_hw_guard_check(void);
const char *ost_hw_guard_error(void);

#endif
