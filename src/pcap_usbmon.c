/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#include "pcap_usbmon.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PCAP_GLOBAL_HEADER_SIZE 24u
#define PCAP_PACKET_HEADER_SIZE 16u
#define USBMON_HEADER_SIZE 48u
#define USBMON_MMAPPED_HEADER_SIZE 64u
#define USBMON_ISO_DESCRIPTOR_SIZE 16u
#define USBMON_LINK_TYPE 189u
#define USBMON_MMAPPED_LINK_TYPE 220u

static void set_error(char *error, size_t error_size, const char *format, ...)
{
    va_list arguments;

    if (error == NULL || error_size == 0u) {
        return;
    }
    va_start(arguments, format);
    (void)vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static uint64_t read_number(const unsigned char *bytes, size_t width, int big_endian)
{
    uint64_t value = 0u;
    size_t i;

    for (i = 0u; i < width; ++i) {
        if (big_endian) {
            value = (value << 8u) | bytes[i];
        } else {
            value |= (uint64_t)bytes[i] << (i * 8u);
        }
    }
    return value;
}

static unsigned char *read_file(const char *path, size_t *size, char *error, size_t error_size)
{
    FILE *file = NULL;
    unsigned char *bytes = NULL;
    long end;

    if (path == NULL) {
        set_error(error, error_size, "pcap path is NULL");
        return NULL;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        set_error(error, error_size, "%s: %s", path, strerror(errno));
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (end = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
        set_error(error, error_size, "%s: cannot determine file size", path);
        fclose(file);
        return NULL;
    }
    *size = (size_t)end;
    if (*size != 0u) {
        bytes = malloc(*size);
        if (bytes == NULL) {
            set_error(error, error_size, "%s: cannot allocate %lu bytes", path, (unsigned long)*size);
            fclose(file);
            return NULL;
        }
        if (fread(bytes, 1u, *size, file) != *size) {
            set_error(error, error_size, "%s: short read", path);
            free(bytes);
            fclose(file);
            return NULL;
        }
    }
    if (fclose(file) != 0) {
        set_error(error, error_size, "%s: cannot close after reading", path);
        free(bytes);
        return NULL;
    }
    return bytes;
}

static int check_size(
    size_t offset,
    size_t required,
    size_t file_size,
    char *error,
    size_t error_size)
{
    if (offset > file_size || required > file_size - offset) {
        set_error(error, error_size, "truncated pcap at file offset %lu", (unsigned long)offset);
        return 0;
    }
    return 1;
}

static int grow_records(ost_pcap_usbmon *capture, size_t *capacity)
{
    ost_usbmon_record *records;
    size_t new_capacity;

    if (capture->count < *capacity) {
        return 1;
    }
    new_capacity = *capacity == 0u ? 16u : *capacity * 2u;
    if (new_capacity < *capacity || new_capacity > SIZE_MAX / sizeof(*records)) {
        return 0;
    }
    records = realloc(capture->records, new_capacity * sizeof(*records));
    if (records == NULL) {
        return 0;
    }
    capture->records = records;
    *capacity = new_capacity;
    return 1;
}

void ost_pcap_usbmon_free(ost_pcap_usbmon *capture)
{
    size_t i;

    if (capture == NULL) {
        return;
    }
    for (i = 0u; i < capture->count; ++i) {
        free(capture->records[i].data);
    }
    free(capture->records);
    free(capture);
}

/*
 * A device answers at address 0 until SET_ADDRESS gives it its address, so records addressed to 0 belong to the
 * address assigned by the next SET_ADDRESS on the same bus. A bus without a later SET_ADDRESS keeps address 0.
 */
static void attribute_default_address(ost_pcap_usbmon *capture)
{
    uint16_t buses[16];
    uint8_t assigned[16];
    size_t pending = 0u;
    size_t i;

    for (i = capture->count; i-- > 0u;) {
        ost_usbmon_record *record = &capture->records[i];
        size_t slot;

        for (slot = 0u; slot < pending && buses[slot] != record->bus_number; ++slot) {
        }
        if (record->setup_present && record->setup[0] == 0u && record->setup[1] == 5u &&
            record->device_address == 0u && record->setup[2] != 0u) {
            if (slot == pending && pending < sizeof(buses) / sizeof(buses[0])) {
                buses[pending] = record->bus_number;
                ++pending;
            }
            if (slot < pending) {
                assigned[slot] = record->setup[2];
            }
        }
        if (record->device_address == 0u && slot < pending) {
            record->device_address = assigned[slot];
        }
    }
}

int ost_pcap_usbmon_load(const char *path, ost_pcap_usbmon **capture_out, char *error, size_t error_size)
{
    static const unsigned char little_micro[4] = {0xd4u, 0xc3u, 0xb2u, 0xa1u};
    static const unsigned char big_micro[4] = {0xa1u, 0xb2u, 0xc3u, 0xd4u};
    static const unsigned char little_nano[4] = {0x4du, 0x3cu, 0xb2u, 0xa1u};
    static const unsigned char big_nano[4] = {0xa1u, 0xb2u, 0x3cu, 0x4du};
    unsigned char *file_data = NULL;
    size_t file_size = 0u;
    size_t offset = PCAP_GLOBAL_HEADER_SIZE;
    size_t capacity = 0u;
    size_t header_size;
    uint32_t snaplen;
    uint32_t network;
    ost_pcap_usbmon *capture;
    int status = OST_ERR_IO;

    if (capture_out == NULL) {
        set_error(error, error_size, "capture output is NULL");
        return OST_ERR_INVALID_ARGUMENT;
    }
    *capture_out = NULL;
    capture = calloc(1u, sizeof(*capture));
    if (capture == NULL) {
        set_error(error, error_size, "cannot allocate capture");
        return OST_ERR_NO_MEMORY;
    }
    if (error != NULL && error_size != 0u) {
        error[0] = '\0';
    }
    file_data = read_file(path, &file_size, error, error_size);
    if (file_data == NULL) {
        goto fail;
    }
    if (!check_size(0u, PCAP_GLOBAL_HEADER_SIZE, file_size, error, error_size)) {
        goto fail;
    }
    if (memcmp(file_data, little_micro, 4u) == 0 || memcmp(file_data, little_nano, 4u) == 0) {
        capture->big_endian = 0;
    } else if (memcmp(file_data, big_micro, 4u) == 0 || memcmp(file_data, big_nano, 4u) == 0) {
        capture->big_endian = 1;
    } else {
        set_error(error, error_size, "unsupported pcap magic");
        goto fail;
    }
    if (read_number(file_data + 4u, 2u, capture->big_endian) != 2u) {
        set_error(error, error_size, "unsupported pcap major version");
        goto fail;
    }
    snaplen = (uint32_t)read_number(file_data + 16u, 4u, capture->big_endian);
    network = (uint32_t)read_number(file_data + 20u, 4u, capture->big_endian);
    capture->link_type = (uint16_t)(network & 0xffffu);
    if (capture->link_type == USBMON_LINK_TYPE) {
        header_size = USBMON_HEADER_SIZE;
    } else if (capture->link_type == USBMON_MMAPPED_LINK_TYPE) {
        header_size = USBMON_MMAPPED_HEADER_SIZE;
    } else {
        set_error(error, error_size, "unsupported pcap link type %u", (unsigned int)capture->link_type);
        goto fail;
    }
    while (offset < file_size) {
        uint32_t captured_size;
        size_t payload_offset;
        size_t descriptor_size = 0u;
        size_t payload_available;
        ost_usbmon_record *record;
        const unsigned char *packet;

        if (!check_size(offset, PCAP_PACKET_HEADER_SIZE, file_size, error, error_size)) {
            goto fail;
        }
        captured_size = (uint32_t)read_number(file_data + offset + 8u, 4u, capture->big_endian);
        offset += PCAP_PACKET_HEADER_SIZE;
        if (captured_size > snaplen) {
            set_error(error, error_size, "pcap frame %lu exceeds snapshot length", (unsigned long)(capture->count + 1u));
            goto fail;
        }
        if (!check_size(offset, captured_size, file_size, error, error_size)) {
            goto fail;
        }
        if (captured_size < header_size) {
            set_error(error, error_size, "usbmon frame %lu has a short header", (unsigned long)(capture->count + 1u));
            goto fail;
        }
        if (!grow_records(capture, &capacity)) {
            status = OST_ERR_NO_MEMORY;
            set_error(error, error_size, "cannot allocate usbmon records");
            goto fail;
        }
        packet = file_data + offset;
        record = &capture->records[capture->count];
        memset(record, 0, sizeof(*record));
        record->id = read_number(packet, 8u, capture->big_endian);
        record->frame_number = capture->count + 1u;
        record->event = packet[8];
        record->transfer_type = packet[9];
        record->endpoint = packet[10];
        record->device_address = packet[11];
        record->bus_number = (uint16_t)read_number(packet + 12u, 2u, capture->big_endian);
        record->status = (int32_t)(uint32_t)read_number(packet + 28u, 4u, capture->big_endian);
        record->length = (uint32_t)read_number(packet + 32u, 4u, capture->big_endian);
        record->captured_length = (uint32_t)read_number(packet + 36u, 4u, capture->big_endian);
        if (record->event != OST_USBMON_SUBMISSION &&
            record->event != OST_USBMON_COMPLETION &&
            record->event != OST_USBMON_SUBMISSION_ERROR) {
            set_error(error, error_size, "usbmon frame %lu has unknown event type", (unsigned long)record->frame_number);
            goto fail;
        }
        if (record->transfer_type > 3u) {
            set_error(error, error_size, "usbmon frame %lu has unknown transfer type", (unsigned long)record->frame_number);
            goto fail;
        }
        record->setup_present = record->event == OST_USBMON_SUBMISSION &&
            record->transfer_type == 2u && packet[14] == 0;
        if (record->setup_present) {
            memcpy(record->setup, packet + 40u, 8u);
        }
        payload_offset = header_size;
        if (header_size == USBMON_MMAPPED_HEADER_SIZE && record->transfer_type == 0u) {
            descriptor_size = (size_t)(uint32_t)read_number(packet + 60u, 4u, capture->big_endian) * USBMON_ISO_DESCRIPTOR_SIZE;
            if (descriptor_size / USBMON_ISO_DESCRIPTOR_SIZE != read_number(packet + 60u, 4u, capture->big_endian)) {
                set_error(error, error_size, "usbmon frame %lu has too many descriptors", (unsigned long)record->frame_number);
                goto fail;
            }
        }
        if (descriptor_size > captured_size - payload_offset) {
            set_error(error, error_size, "usbmon frame %lu has truncated descriptors", (unsigned long)record->frame_number);
            goto fail;
        }
        payload_offset += descriptor_size;
        payload_available = captured_size - payload_offset;
        /*
         * QEMU's usb-host writer sets the data flag to '=' and counts the header in the captured length even when
         * payload bytes follow. The actual bytes remaining after the header therefore determine the payload size.
         */
        if (packet[15] == 0 && record->captured_length > payload_available) {
            set_error(error, error_size, "usbmon frame %lu has truncated data", (unsigned long)record->frame_number);
            goto fail;
        }
        if (payload_available != 0u) {
            record->data = malloc(payload_available);
            if (record->data == NULL) {
                status = OST_ERR_NO_MEMORY;
                set_error(error, error_size, "cannot allocate usbmon frame data");
                goto fail;
            }
            memcpy(record->data, packet + payload_offset, payload_available);
            record->data_size = payload_available;
        }
        ++capture->count;
        offset += captured_size;
    }
    attribute_default_address(capture);
    free(file_data);
    *capture_out = capture;
    return OST_SUCCESS;

fail:
    free(file_data);
    ost_pcap_usbmon_free(capture);
    return status;
}
