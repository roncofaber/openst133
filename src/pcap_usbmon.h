/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#ifndef OPENST133_PCAP_USBMON_H
#define OPENST133_PCAP_USBMON_H

#include "openst133.h"

#include <stddef.h>
#include <stdint.h>

typedef enum ost_usbmon_event {
    OST_USBMON_SUBMISSION = 'S',
    OST_USBMON_COMPLETION = 'C',
    OST_USBMON_SUBMISSION_ERROR = 'E'
} ost_usbmon_event;

typedef struct ost_usbmon_record {
    uint64_t id;
    uint64_t frame_number;
    uint8_t event;
    uint8_t transfer_type;
    uint8_t endpoint;
    uint8_t device_address;
    uint16_t bus_number;
    int32_t status;
    uint32_t length;
    uint32_t captured_length;
    int setup_present;
    unsigned char setup[8];
    unsigned char *data;
    size_t data_size;
} ost_usbmon_record;

typedef struct ost_pcap_usbmon {
    uint16_t link_type;
    int big_endian;
    ost_usbmon_record *records;
    size_t count;
} ost_pcap_usbmon;

int ost_pcap_usbmon_load(const char *path, ost_pcap_usbmon **capture, char *error, size_t error_size);
void ost_pcap_usbmon_free(ost_pcap_usbmon *capture);

typedef struct ost_replay_transport ost_replay_transport;

int ost_replay_transport_create(
    const char *path,
    ost_replay_transport **replay,
    char *error,
    size_t error_size);
void ost_replay_transport_destroy(ost_replay_transport *replay);
const ost_transport *ost_replay_transport_interface(ost_replay_transport *replay);
const char *ost_replay_transport_last_error(const ost_replay_transport *replay);

#endif
