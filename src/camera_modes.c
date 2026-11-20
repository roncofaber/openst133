/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#include "camera_modes.h"


int ost_camera_modes_table_info(size_t slot, uint16_t *command, size_t *count)
{
    static const uint16_t commands[4] = {0x20u, 0x20u, 0x20u, 0x48u};
    static const size_t counts[4] = {1359u, 4136u, 1368u, 32768u};

    if (command == NULL || count == NULL || slot < 4u || slot >= 8u) {
        return 0;
    }
    *command = commands[slot - 4u];
    *count = counts[slot - 4u];
    return 1;
}

const unsigned char *ost_camera_modes_tables_sha256(void)
{
    static const unsigned char digest[32] = {
        0x30, 0x77, 0xbf, 0xd0, 0x48, 0xe3, 0x4f, 0xdc,
        0x5b, 0xc8, 0x80, 0x50, 0x63, 0x4c, 0x78, 0xec,
        0x89, 0x15, 0x8d, 0xc4, 0x4b, 0xfc, 0x1d, 0x31,
        0x8f, 0xa7, 0x37, 0x93, 0x67, 0xf6, 0x53, 0x16,
    };

    return digest;
}
