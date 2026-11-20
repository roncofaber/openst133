/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#include "pcap_usbmon.h"
#include "transport_internal.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ost_replay_entry {
    const ost_usbmon_record *submission;
    const ost_usbmon_record *completion;
    int discovery;
} ost_replay_entry;

struct ost_replay_transport {
    ost_transport interface;
    ost_pcap_usbmon *capture;
    ost_replay_entry *entries;
    size_t entry_count;
    size_t next_entry;
    ost_device_info *devices;
    size_t device_count;
    int handle_open;
    char error[512];
};

typedef struct ost_replay_handle {
    ost_replay_transport *replay;
    ost_device_info info;
} ost_replay_handle;

static void ost_replay_transport_initialize(ost_replay_transport *replay);

static void set_error(ost_replay_transport *replay, const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    (void)vsnprintf(replay->error, sizeof(replay->error), format, arguments);
    va_end(arguments);
}

static void set_create_error(char *error, size_t error_size, const char *format, ...)
{
    va_list arguments;

    if (error == NULL || error_size == 0u) {
        return;
    }
    va_start(arguments, format);
    (void)vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static uint16_t setup_u16(const unsigned char *setup, size_t offset)
{
    return (uint16_t)((uint16_t)setup[offset] | ((uint16_t)setup[offset + 1u] << 8u));
}

static const ost_usbmon_record *find_completion(const ost_pcap_usbmon *capture, size_t submission_index)
{
    const ost_usbmon_record *submission = &capture->records[submission_index];
    size_t i;

    for (i = submission_index + 1u; i < capture->count; ++i) {
        const ost_usbmon_record *candidate = &capture->records[i];

        if (candidate->event == OST_USBMON_SUBMISSION ||
            candidate->id != submission->id ||
            candidate->transfer_type != submission->transfer_type ||
            candidate->endpoint != submission->endpoint ||
            candidate->bus_number != submission->bus_number ||
            candidate->device_address != submission->device_address) {
            continue;
        }
        return candidate;
    }
    return NULL;
}

static int is_device_descriptor(const ost_usbmon_record *submission, const ost_usbmon_record *completion)
{
    if (!submission->setup_present || submission->endpoint != 0x80u ||
        submission->setup[0] != 0x80u || submission->setup[1] != 6u ||
        submission->setup[2] != 0u || submission->setup[3] != 1u ||
        setup_u16(submission->setup, 4u) != 0u || completion == NULL ||
        completion->event != OST_USBMON_COMPLETION || completion->status != 0 ||
        completion->length < 12u || completion->data_size < 12u) {
        return 0;
    }
    return 1;
}

static int add_device(ost_replay_transport *replay, const ost_usbmon_record *submission, const ost_usbmon_record *completion)
{
    ost_device_info *devices;
    size_t i;

    for (i = 0u; i < replay->device_count; ++i) {
        if (replay->devices[i].bus_number == submission->bus_number &&
            replay->devices[i].device_address == submission->device_address) {
            return 1;
        }
    }
    if (submission->bus_number > 255u) {
        return 0;
    }
    devices = realloc(replay->devices, (replay->device_count + 1u) * sizeof(*devices));
    if (devices == NULL) {
        return -1;
    }
    replay->devices = devices;
    devices = &replay->devices[replay->device_count];
    memset(devices, 0, sizeof(*devices));
    devices->vendor_id = setup_u16(completion->data, 8u);
    devices->product_id = setup_u16(completion->data, 10u);
    devices->bus_number = (uint8_t)submission->bus_number;
    devices->device_address = submission->device_address;
    ++replay->device_count;
    return 1;
}

int ost_replay_transport_create(
    const char *path,
    ost_replay_transport **replay_out,
    char *error,
    size_t error_size)
{
    ost_replay_transport *replay;
    size_t i;
    int status;

    if (replay_out == NULL) {
        set_create_error(error, error_size, "replay output is NULL");
        return OST_ERR_INVALID_ARGUMENT;
    }
    *replay_out = NULL;
    if (error != NULL && error_size != 0u) {
        error[0] = '\0';
    }
    replay = calloc(1u, sizeof(*replay));
    if (replay == NULL) {
        set_create_error(error, error_size, "cannot allocate replay transport");
        return OST_ERR_NO_MEMORY;
    }
    status = ost_pcap_usbmon_load(path, &replay->capture, error, error_size);
    if (status != OST_SUCCESS) {
        free(replay);
        return status;
    }
    if (replay->capture->count != 0u) {
        replay->entries = calloc(replay->capture->count, sizeof(*replay->entries));
        if (replay->entries == NULL) {
            set_create_error(error, error_size, "cannot allocate replay entries");
            ost_replay_transport_destroy(replay);
            return OST_ERR_NO_MEMORY;
        }
    }
    for (i = 0u; i < replay->capture->count; ++i) {
        const ost_usbmon_record *submission = &replay->capture->records[i];
        const ost_usbmon_record *completion;

        if (submission->event != OST_USBMON_SUBMISSION) {
            continue;
        }
        completion = find_completion(replay->capture, i);
        if (is_device_descriptor(submission, completion)) {
            int added = add_device(replay, submission, completion);

            if (added == 0) {
                set_create_error(error, error_size, "recorded bus number exceeds transport range");
                ost_replay_transport_destroy(replay);
                return OST_ERR_IO;
            }
            if (added < 0) {
                set_create_error(error, error_size, "cannot allocate replay devices");
                ost_replay_transport_destroy(replay);
                return OST_ERR_NO_MEMORY;
            }
        }
        replay->entries[replay->entry_count].submission = submission;
        replay->entries[replay->entry_count].completion = completion;
        replay->entries[replay->entry_count].discovery = is_device_descriptor(submission, completion);
        ++replay->entry_count;
    }
    replay->interface.userdata = replay;
    ost_replay_transport_initialize(replay);
    *replay_out = replay;
    return OST_SUCCESS;
}

void ost_replay_transport_destroy(ost_replay_transport *replay)
{
    if (replay == NULL) {
        return;
    }
    ost_pcap_usbmon_free(replay->capture);
    free(replay->entries);
    free(replay->devices);
    free(replay);
}

const ost_transport *ost_replay_transport_interface(ost_replay_transport *replay)
{
    return replay == NULL ? NULL : &replay->interface;
}

const char *ost_replay_transport_last_error(const ost_replay_transport *replay)
{
    return replay == NULL ? "invalid replay transport" : replay->error;
}

static int replay_list_devices(void *userdata, ost_device_info **devices, size_t *count)
{
    ost_replay_transport *replay = userdata;

    *devices = NULL;
    *count = 0u;
    if (replay->device_count == 0u) {
        return OST_SUCCESS;
    }
    *devices = malloc(replay->device_count * sizeof(**devices));
    if (*devices == NULL) {
        return OST_ERR_NO_MEMORY;
    }
    memcpy(*devices, replay->devices, replay->device_count * sizeof(**devices));
    *count = replay->device_count;
    return OST_SUCCESS;
}

static void replay_free_devices(void *userdata, ost_device_info *devices)
{
    (void)userdata;
    free(devices);
}

static int replay_open(void *userdata, const ost_device_info *info, ost_transport_handle **handle)
{
    ost_replay_transport *replay = userdata;
    ost_replay_handle *opened;
    size_t i;

    *handle = NULL;
    for (i = 0u; i < replay->device_count; ++i) {
        if (memcmp(&replay->devices[i], info, sizeof(*info)) == 0) {
            if (replay->handle_open) {
                set_error(replay, "a replay transport plays one recorded sequence to one open handle at a time");
                return OST_ERR_BUSY;
            }
            opened = calloc(1u, sizeof(*opened));
            if (opened == NULL) {
                return OST_ERR_NO_MEMORY;
            }
            opened->replay = replay;
            opened->info = *info;
            replay->handle_open = 1;
            *handle = (ost_transport_handle *)opened;
            return OST_SUCCESS;
        }
    }
    set_error(replay, "recorded device %u/%u was not found", (unsigned int)info->bus_number, (unsigned int)info->device_address);
    return OST_ERR_NOT_FOUND;
}

static void replay_close(void *userdata, ost_transport_handle *handle)
{
    ost_replay_transport *replay = userdata;

    if (handle != NULL) {
        replay->handle_open = 0;
    }
    free(handle);
}

static ost_replay_entry *next_matching_entry(
    ost_replay_handle *handle,
    uint8_t transfer_type,
    const unsigned char *control_setup)
{
    ost_replay_transport *replay = handle->replay;
    ost_replay_entry *entry;
    const ost_usbmon_record *submission;

    replay->error[0] = '\0';
    while (replay->next_entry < replay->entry_count && replay->entries[replay->next_entry].discovery &&
           (control_setup == NULL || !replay->entries[replay->next_entry].submission->setup_present ||
            memcmp(replay->entries[replay->next_entry].submission->setup, control_setup, 8u) != 0)) {
        ++replay->next_entry;
    }
    if (replay->next_entry == replay->entry_count) {
        set_error(replay, "replay end of capture: no recorded submission remains");
        return NULL;
    }
    entry = &replay->entries[replay->next_entry];
    submission = entry->submission;
    if (submission->transfer_type != transfer_type) {
        set_error(replay, "replay frame %llu mismatch: recorded transfer type %u, requested %u",
            (unsigned long long)submission->frame_number,
            (unsigned int)submission->transfer_type, (unsigned int)transfer_type);
        return NULL;
    }
    if (submission->bus_number != handle->info.bus_number ||
        submission->device_address != handle->info.device_address) {
        set_error(replay, "replay frame %llu mismatch: recorded device %u/%u, opened %u/%u",
            (unsigned long long)submission->frame_number,
            (unsigned int)submission->bus_number, (unsigned int)submission->device_address,
            (unsigned int)handle->info.bus_number, (unsigned int)handle->info.device_address);
        return NULL;
    }
    return entry;
}

static int check_output(const ost_usbmon_record *submission, const unsigned char *data, uint32_t length)
{
    uint32_t recorded = submission->length != 0u ? submission->length : (uint32_t)submission->data_size;

    if (recorded != length) {
        return 0;
    }
    if (length == 0u) {
        return 1;
    }
    if (data == NULL || submission->data_size < length) {
        return -1;
    }
    return memcmp(data, submission->data, length) == 0 ? 1 : 0;
}

static int map_recorded_status(int32_t status)
{
    /* usbmon completion statuses are Linux errno values on every replay host. */
    switch (-status) {
    case 110:
        return OST_TRANSPORT_ERR_TIMEOUT;
    case 12:
        return OST_ERR_NO_MEMORY;
    case 19:
    case 2:
        return OST_ERR_NOT_FOUND;
    case 38:
    case 95:
        return OST_ERR_UNSUPPORTED;
    default:
        return OST_ERR_IO;
    }
}

static int finish_transfer(
    ost_replay_handle *handle,
    ost_replay_entry *entry,
    unsigned char *data,
    uint32_t capacity,
    int *transferred)
{
    ost_replay_transport *replay = handle->replay;
    const ost_usbmon_record *submission = entry->submission;
    const ost_usbmon_record *completion = entry->completion;

    if (completion == NULL) {
        set_error(replay, "replay frame %llu has no recorded completion", (unsigned long long)submission->frame_number);
        return OST_ERR_IO;
    }
    if (completion->event == OST_USBMON_SUBMISSION_ERROR || completion->status != 0) {
        set_error(replay, "replay frame %llu completion status %d",
            (unsigned long long)submission->frame_number, (int)completion->status);
        ++replay->next_entry;
        return map_recorded_status(completion->status);
    }
    if (completion->length > capacity) {
        set_error(replay, "replay frame %llu has %u response bytes for a %u-byte buffer",
            (unsigned long long)submission->frame_number,
            (unsigned int)completion->length, (unsigned int)capacity);
        return OST_ERR_IO;
    }
    if ((submission->endpoint & 0x80u) != 0u && completion->length != 0u) {
        if (data == NULL || completion->data_size < completion->length) {
            set_error(replay, "replay frame %llu has truncated completion data",
                (unsigned long long)submission->frame_number);
            return OST_ERR_IO;
        }
        memcpy(data, completion->data, completion->length);
    }
    *transferred = (int)completion->length;
    ++replay->next_entry;
    return OST_SUCCESS;
}

static int replay_control(
    void *userdata,
    ost_transport_handle *handle,
    uint8_t request_type,
    uint8_t request,
    uint16_t value,
    uint16_t index,
    unsigned char *data,
    uint16_t length,
    unsigned int timeout_ms,
    int *transferred)
{
    ost_replay_handle *opened = (ost_replay_handle *)handle;
    ost_replay_entry *entry;
    const ost_usbmon_record *submission;
    unsigned char setup[8];
    int output_status;

    (void)userdata;
    (void)timeout_ms;
    if ((length != 0u && data == NULL) || transferred == NULL) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    setup[0] = request_type;
    setup[1] = request;
    setup[2] = (unsigned char)(value & 0xffu);
    setup[3] = (unsigned char)(value >> 8u);
    setup[4] = (unsigned char)(index & 0xffu);
    setup[5] = (unsigned char)(index >> 8u);
    setup[6] = (unsigned char)(length & 0xffu);
    setup[7] = (unsigned char)(length >> 8u);
    entry = next_matching_entry(opened, 2u, setup);
    if (entry == NULL) {
        return OST_ERR_IO;
    }
    submission = entry->submission;
    if (!submission->setup_present || submission->endpoint != (request_type & 0x80u) ||
        memcmp(submission->setup, setup, sizeof(setup)) != 0) {
        set_error(opened->replay, "replay frame %llu mismatch: control endpoint or setup packet",
            (unsigned long long)submission->frame_number);
        return OST_ERR_IO;
    }
    if ((request_type & 0x80u) == 0u) {
        output_status = check_output(submission, data, length);
        if (output_status != 1) {
            set_error(opened->replay, "replay frame %llu mismatch: control OUT data or captured length",
                (unsigned long long)submission->frame_number);
            return OST_ERR_IO;
        }
    }
    return finish_transfer(opened, entry, data, length, transferred);
}

static int replay_bulk(
    void *userdata,
    ost_transport_handle *handle,
    unsigned char endpoint,
    unsigned char *data,
    int length,
    unsigned int timeout_ms,
    int *transferred)
{
    ost_replay_handle *opened = (ost_replay_handle *)handle;
    ost_replay_entry *entry;
    const ost_usbmon_record *submission;

    (void)userdata;
    (void)timeout_ms;
    if (length < 0 || (length != 0 && data == NULL) || transferred == NULL) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    entry = next_matching_entry(opened, 3u, NULL);
    if (entry == NULL) {
        return OST_ERR_IO;
    }
    submission = entry->submission;
    if (submission->endpoint != endpoint) {
        set_error(opened->replay, "replay frame %llu mismatch: recorded bulk endpoint 0x%02x, requested 0x%02x",
            (unsigned long long)submission->frame_number,
            (unsigned int)submission->endpoint, (unsigned int)endpoint);
        return OST_ERR_IO;
    }
    if ((endpoint & 0x80u) == 0u) {
        int output_status = check_output(submission, data, (uint32_t)length);

        if (output_status != 1) {
            set_error(opened->replay, "replay frame %llu mismatch: bulk OUT data or captured length",
                (unsigned long long)submission->frame_number);
            return OST_ERR_IO;
        }
    } else if ((uint32_t)length < submission->length) {
        set_error(opened->replay, "replay frame %llu mismatch: bulk IN buffer is shorter than the recorded request",
            (unsigned long long)submission->frame_number);
        return OST_ERR_IO;
    }
    return finish_transfer(opened, entry, data, (uint32_t)length, transferred);
}

static void ost_replay_transport_initialize(ost_replay_transport *replay)
{
    replay->interface.list_devices = replay_list_devices;
    replay->interface.free_devices = replay_free_devices;
    replay->interface.open = replay_open;
    replay->interface.close = replay_close;
    replay->interface.control_transfer = replay_control;
    replay->interface.bulk_transfer = replay_bulk;
}
