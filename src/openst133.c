/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#include "openst133.h"
#include "transport_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ost_device {
    ost_transport transport;
    ost_transport_handle *handle;
    ost_device_info info;
};

static _Thread_local char open_error_text[512];

const char *ost_open_error(void)
{
    return open_error_text;
}

static int open_failed(const ost_transport *transport, int status)
{
    const char *detail = transport == ost_libusb_transport() ? ost_libusb_open_error() : "";

    (void)snprintf(open_error_text, sizeof(open_error_text), "%s", detail[0] != '\0' ? detail : ost_status_text(status));
    return status;
}

static const ost_transport *select_transport(const ost_transport *transport)
{
    return transport == NULL ? ost_libusb_transport() : transport;
}

static int compare_devices(const void *left_pointer, const void *right_pointer)
{
    const ost_device_info *left = left_pointer;
    const ost_device_info *right = right_pointer;
    size_t shared;
    size_t i;

    if (left->bus_number != right->bus_number) {
        return left->bus_number < right->bus_number ? -1 : 1;
    }
    shared = left->port_count < right->port_count ? left->port_count : right->port_count;
    for (i = 0u; i < shared; ++i) {
        if (left->port_numbers[i] != right->port_numbers[i]) {
            return left->port_numbers[i] < right->port_numbers[i] ? -1 : 1;
        }
    }
    if (left->port_count != right->port_count) {
        return left->port_count < right->port_count ? -1 : 1;
    }
    if (left->device_address != right->device_address) {
        return left->device_address < right->device_address ? -1 : 1;
    }
    return 0;
}

int ost_list_devices(const ost_transport *transport, ost_device_info **devices, size_t *count)
{
    ost_device_info *available = NULL;
    ost_device_info *matches;
    size_t available_count = 0u;
    size_t match_count = 0u;
    size_t i;
    int status;

    if (devices == NULL || count == NULL) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    *devices = NULL;
    *count = 0u;
    transport = select_transport(transport);
    if (transport->list_devices == NULL || transport->free_devices == NULL) {
        return OST_ERR_UNSUPPORTED;
    }
    status = transport->list_devices(transport->userdata, &available, &available_count);
    if (status != OST_SUCCESS) {
        return status;
    }
    if (available_count > 0u && available == NULL) {
        return OST_ERR_IO;
    }
    if (available_count == 0u) {
        if (available != NULL) {
            transport->free_devices(transport->userdata, available);
        }
        return OST_SUCCESS;
    }
    matches = malloc(available_count * sizeof(*matches));
    if (matches == NULL) {
        transport->free_devices(transport->userdata, available);
        return OST_ERR_NO_MEMORY;
    }
    for (i = 0u; i < available_count; ++i) {
        if (available[i].vendor_id == OST_USB_VENDOR_ID &&
            available[i].product_id == OST_USB_PRODUCT_ID) {
            matches[match_count] = available[i];
            if (matches[match_count].port_count > OST_MAX_PORT_DEPTH) {
                matches[match_count].port_count = 0u;
            }
            memset(matches[match_count].port_numbers + matches[match_count].port_count, 0,
                OST_MAX_PORT_DEPTH - matches[match_count].port_count);
            ++match_count;
        }
    }
    transport->free_devices(transport->userdata, available);
    if (match_count == 0u) {
        free(matches);
        return OST_SUCCESS;
    }
    qsort(matches, match_count, sizeof(*matches), compare_devices);
    *devices = matches;
    *count = match_count;
    return OST_SUCCESS;
}

void ost_free_devices(ost_device_info *devices)
{
    free(devices);
}

int ost_device_port_path(const ost_device_info *info, char *buffer, size_t size)
{
    size_t used;
    size_t i;
    int written;

    if (info == NULL || buffer == NULL || size == 0u || info->port_count > OST_MAX_PORT_DEPTH) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    buffer[0] = '\0';
    if (info->port_count == 0u) {
        return OST_ERR_UNSUPPORTED;
    }
    written = snprintf(buffer, size, "%u", (unsigned int)info->bus_number);
    for (i = 0u; written >= 0 && (size_t)written < size && i < info->port_count; ++i) {
        used = (size_t)written;
        written = snprintf(buffer + used, size - used, "%c%u", i == 0u ? '-' : '.',
            (unsigned int)info->port_numbers[i]);
        if (written >= 0) {
            written += (int)used;
        }
    }
    if (written < 0 || (size_t)written >= size) {
        buffer[0] = '\0';
        return OST_ERR_BUFFER_TOO_SMALL;
    }
    return OST_SUCCESS;
}

static int parse_number(const char **text, unsigned long limit, unsigned long *value)
{
    const char *cursor = *text;
    unsigned long parsed = 0u;

    if (*cursor < '0' || *cursor > '9') {
        return 0;
    }
    while (*cursor >= '0' && *cursor <= '9') {
        parsed = parsed * 10u + (unsigned long)(*cursor - '0');
        if (parsed > limit) {
            return 0;
        }
        ++cursor;
    }
    *text = cursor;
    *value = parsed;
    return 1;
}

int ost_select_device(const ost_device_info *devices, size_t count, const char *selector, size_t *index)
{
    const char *cursor = selector;
    unsigned long first;
    unsigned long value;
    uint8_t ports[OST_MAX_PORT_DEPTH];
    size_t port_count = 0u;
    size_t i;

    if (index == NULL || (devices == NULL && count > 0u)) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    if (selector == NULL || selector[0] == '\0') {
        if (count == 0u) {
            return OST_ERR_NOT_FOUND;
        }
        if (count > 1u) {
            return OST_ERR_INVALID_ARGUMENT;
        }
        *index = 0u;
        return OST_SUCCESS;
    }
    if (!parse_number(&cursor, 0xfffffffful, &first)) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    if (*cursor == '\0') {
        if (first >= count) {
            return OST_ERR_NOT_FOUND;
        }
        *index = (size_t)first;
        return OST_SUCCESS;
    }
    if (first > 255u) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    if (*cursor == ':') {
        ++cursor;
        if (!parse_number(&cursor, 255u, &value) || *cursor != '\0') {
            return OST_ERR_INVALID_ARGUMENT;
        }
        for (i = 0u; i < count; ++i) {
            if (devices[i].bus_number == first && devices[i].device_address == value) {
                *index = i;
                return OST_SUCCESS;
            }
        }
        return OST_ERR_NOT_FOUND;
    }
    if (*cursor != '-') {
        return OST_ERR_INVALID_ARGUMENT;
    }
    do {
        ++cursor;
        if (port_count == OST_MAX_PORT_DEPTH || !parse_number(&cursor, 255u, &value)) {
            return OST_ERR_INVALID_ARGUMENT;
        }
        ports[port_count++] = (uint8_t)value;
    } while (*cursor == '.');
    if (*cursor != '\0') {
        return OST_ERR_INVALID_ARGUMENT;
    }
    for (i = 0u; i < count; ++i) {
        if (devices[i].bus_number == first && devices[i].port_count == port_count &&
            memcmp(devices[i].port_numbers, ports, port_count) == 0) {
            *index = i;
            return OST_SUCCESS;
        }
    }
    return OST_ERR_NOT_FOUND;
}

int ost_open(const ost_transport *transport, const ost_device_info *info, ost_device **device)
{
    ost_transport_handle *handle = NULL;
    ost_device *opened;
    int status;

    open_error_text[0] = '\0';
    if (info == NULL || device == NULL) {
        return open_failed(NULL, OST_ERR_INVALID_ARGUMENT);
    }
    *device = NULL;
    if (info->vendor_id != OST_USB_VENDOR_ID || info->product_id != OST_USB_PRODUCT_ID) {
        return open_failed(NULL, OST_ERR_NOT_FOUND);
    }
    transport = select_transport(transport);
    if (transport->open == NULL || transport->close == NULL) {
        return open_failed(NULL, OST_ERR_UNSUPPORTED);
    }
    status = transport->open(transport->userdata, info, &handle);
    if (status != OST_SUCCESS) {
        return open_failed(transport, status);
    }
    if (handle == NULL) {
        return open_failed(NULL, OST_ERR_IO);
    }
    opened = calloc(1u, sizeof(*opened));
    if (opened == NULL) {
        transport->close(transport->userdata, handle);
        return open_failed(NULL, OST_ERR_NO_MEMORY);
    }
    opened->transport = *transport;
    opened->handle = handle;
    opened->info = *info;
    *device = opened;
    return OST_SUCCESS;
}

int ost_device_swap_transport(ost_device *device, ost_transport *transport, ost_transport_handle **handle)
{
    ost_transport previous;
    ost_transport_handle *previous_handle;

    if (device == NULL || transport == NULL || handle == NULL) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    previous = device->transport;
    previous_handle = device->handle;
    device->transport = *transport;
    device->handle = *handle;
    *transport = previous;
    *handle = previous_handle;
    return OST_SUCCESS;
}

void ost_close(ost_device *device)
{
    if (device == NULL) {
        return;
    }
    device->transport.close(device->transport.userdata, device->handle);
    free(device);
}

int ost_get_info(ost_device *device, ost_device_info *info)
{
    if (device == NULL || info == NULL) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    *info = device->info;
    return OST_SUCCESS;
}

int ost_get_temperature(ost_device *device, double *celsius)
{
    if (device == NULL || celsius == NULL) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    return OST_ERR_UNSUPPORTED;
}

int ost_set_temperature(ost_device *device, double celsius)
{
    (void)celsius;
    return device == NULL ? OST_ERR_INVALID_ARGUMENT : OST_ERR_UNSUPPORTED;
}

int ost_get_shutter(ost_device *device, ost_shutter_state *state)
{
    if (device == NULL || state == NULL) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    return OST_ERR_UNSUPPORTED;
}

int ost_set_shutter(ost_device *device, ost_shutter_state state)
{
    (void)state;
    return device == NULL ? OST_ERR_INVALID_ARGUMENT : OST_ERR_UNSUPPORTED;
}

int ost_get_exposure(ost_device *device, uint32_t *exposure_ms)
{
    if (device == NULL || exposure_ms == NULL) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    return OST_ERR_UNSUPPORTED;
}

int ost_set_exposure(ost_device *device, uint32_t exposure_ms)
{
    (void)exposure_ms;
    return device == NULL ? OST_ERR_INVALID_ARGUMENT : OST_ERR_UNSUPPORTED;
}

int ost_get_readout_params(ost_device *device, ost_readout_params *params)
{
    if (device == NULL || params == NULL) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    return OST_ERR_UNSUPPORTED;
}

int ost_set_readout_params(ost_device *device, const ost_readout_params *params)
{
    if (device == NULL || params == NULL) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    return OST_ERR_UNSUPPORTED;
}

int ost_acquire(ost_device *device, void *buffer, size_t buffer_size, size_t *acquired_size)
{
    (void)buffer;
    (void)buffer_size;
    (void)acquired_size;
    return device == NULL ? OST_ERR_INVALID_ARGUMENT : OST_ERR_UNSUPPORTED;
}

int ost_abort(ost_device *device)
{
    return device == NULL ? OST_ERR_INVALID_ARGUMENT : OST_ERR_UNSUPPORTED;
}

int ost_control_transfer(
    ost_device *device,
    uint8_t request_type,
    uint8_t request,
    uint16_t value,
    uint16_t index,
    unsigned char *data,
    uint16_t length,
    unsigned int timeout_ms,
    int *transferred)
{
    if (device == NULL || transferred == NULL) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    if (device->transport.control_transfer == NULL) {
        return OST_ERR_UNSUPPORTED;
    }
    int status = device->transport.control_transfer(
        device->transport.userdata,
        device->handle,
        request_type,
        request,
        value,
        index,
        data,
        length,
        timeout_ms,
        transferred);
    return status == OST_TRANSPORT_ERR_TIMEOUT ? OST_ERR_TIMEOUT : status;
}

int ost_bulk_transfer(
    ost_device *device,
    unsigned char endpoint,
    unsigned char *data,
    int length,
    unsigned int timeout_ms,
    int *transferred)
{
    if (device == NULL || transferred == NULL || length < 0) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    if (device->transport.bulk_transfer == NULL) {
        return OST_ERR_UNSUPPORTED;
    }
    int status = device->transport.bulk_transfer(
        device->transport.userdata,
        device->handle,
        endpoint,
        data,
        length,
        timeout_ms,
        transferred);
    return status == OST_TRANSPORT_ERR_TIMEOUT ? OST_ERR_TIMEOUT : status;
}
