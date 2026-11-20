/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#include "camera_stream.h"
#include "hw_guard.h"
#include "transport_internal.h"
#include "time_util.h"

#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __linux__
#include <dirent.h>
#include <limits.h>
#include <unistd.h>
#endif

struct libusb_handle {
    libusb_context *context;
    libusb_device_handle *device;
    int interface_claimed;
};

/* The supported USB interface is vendor-specific interface 0. */
#define OST_LIBUSB_INTERFACE 0

#ifndef OST_LIBUSB_PROC_ROOT
#define OST_LIBUSB_PROC_ROOT "/proc"
#endif

static _Thread_local char libusb_open_error[512];

const char *ost_libusb_open_error(void)
{
    return libusb_open_error;
}

#ifdef __linux__
static int find_usbfs_holder(const char *device_path, long *holder_pid, char *holder_name, size_t name_size)
{
    DIR *processes = opendir(OST_LIBUSB_PROC_ROOT);
    struct dirent *process;

    if (processes == NULL) {
        return 0;
    }
    while ((process = readdir(processes)) != NULL) {
        char *end = NULL;
        long pid = strtol(process->d_name, &end, 10);
        char path[PATH_MAX + 384];
        DIR *descriptors;
        struct dirent *descriptor;

        if (process->d_name[0] == '\0' || end == process->d_name || *end != '\0' || pid <= 0 ||
            pid == (long)getpid()) {
            continue;
        }
        (void)snprintf(path, sizeof(path), "%s/%ld/fd", OST_LIBUSB_PROC_ROOT, pid);
        descriptors = opendir(path);
        if (descriptors == NULL) {
            continue;
        }
        while ((descriptor = readdir(descriptors)) != NULL) {
            char target[PATH_MAX + 1];
            ssize_t length;

            (void)snprintf(path, sizeof(path), "%s/%ld/fd/%s", OST_LIBUSB_PROC_ROOT, pid, descriptor->d_name);
            length = readlink(path, target, sizeof(target) - 1u);
            if (length < 0 || (size_t)length >= sizeof(target)) {
                continue;
            }
            target[length] = '\0';
            if (strcmp(target, device_path) == 0) {
                FILE *comm;

                (void)snprintf(holder_name, name_size, "%s", "unknown");
                (void)snprintf(path, sizeof(path), "%s/%ld/comm", OST_LIBUSB_PROC_ROOT, pid);
                comm = fopen(path, "rb");
                if (comm != NULL) {
                    if (fgets(holder_name, (int)name_size, comm) != NULL) {
                        holder_name[strcspn(holder_name, "\r\n")] = '\0';
                    }
                    (void)fclose(comm);
                }
                (void)closedir(descriptors);
                (void)closedir(processes);
                *holder_pid = pid;
                return 1;
            }
        }
        (void)closedir(descriptors);
    }
    (void)closedir(processes);
    return 0;
}
#endif

static void set_claim_error(int error, const ost_device_info *info)
{
    char device_path[64];

    (void)snprintf(device_path, sizeof(device_path), "/dev/bus/usb/%03u/%03u",
        (unsigned int)info->bus_number, (unsigned int)info->device_address);
    if (error == LIBUSB_ERROR_ACCESS) {
#ifdef _WIN32
        (void)snprintf(libusb_open_error, sizeof(libusb_open_error),
            "permission denied opening camera USB interface %u; bind the camera to the WinUSB driver (see docs/user/windows.md)",
            OST_LIBUSB_INTERFACE);
#else
        (void)snprintf(libusb_open_error, sizeof(libusb_open_error),
            "permission denied opening camera USB interface %u on %s; run 'openst133 setup-udev' once to install the udev rule",
            OST_LIBUSB_INTERFACE, device_path);
#endif
        return;
    }
#ifdef __linux__
    if (error == LIBUSB_ERROR_BUSY) {
        long holder_pid = 0;
        char holder_name[64];

        if (find_usbfs_holder(device_path, &holder_pid, holder_name, sizeof(holder_name))) {
            (void)snprintf(libusb_open_error, sizeof(libusb_open_error),
                "camera USB interface %u on %s is busy; held by pid %ld (%s); close the holder or VM and retry",
                OST_LIBUSB_INTERFACE, device_path, holder_pid, holder_name);
            return;
        }
    }
#endif
    if (error == LIBUSB_ERROR_BUSY) {
        (void)snprintf(libusb_open_error, sizeof(libusb_open_error),
            "camera USB interface %u on %s is busy; another process or kernel driver holds it; "
            "close the other ost process or VM and retry",
            OST_LIBUSB_INTERFACE, device_path);
    }
}

static int from_libusb_error(int error)
{
    switch (error) {
    case LIBUSB_ERROR_TIMEOUT:
        return OST_TRANSPORT_ERR_TIMEOUT;
    case LIBUSB_ERROR_NO_MEM:
        return OST_ERR_NO_MEMORY;
    case LIBUSB_ERROR_NO_DEVICE:
        return OST_ERR_NOT_FOUND;
    case LIBUSB_ERROR_NOT_SUPPORTED:
        return OST_ERR_UNSUPPORTED;
    case LIBUSB_ERROR_BUSY:
        return OST_ERR_BUSY;
    case LIBUSB_ERROR_ACCESS:
        return OST_ERR_ACCESS;
    default:
        return OST_ERR_IO;
    }
}

static int libusb_list_devices(void *userdata, ost_device_info **devices, size_t *count)
{
    libusb_context *context = NULL;
    libusb_device **list = NULL;
    ost_device_info *result = NULL;
    ssize_t device_count;
    ssize_t i;
    int status;

    (void)userdata;
    *devices = NULL;
    *count = 0u;
    status = libusb_init(&context);
    if (status != 0) {
        return from_libusb_error(status);
    }
    device_count = libusb_get_device_list(context, &list);
    if (device_count < 0) {
        status = from_libusb_error((int)device_count);
        libusb_exit(context);
        return status;
    }
    if (device_count == 0) {
        libusb_free_device_list(list, 1);
        libusb_exit(context);
        return OST_SUCCESS;
    }
    result = calloc((size_t)device_count, sizeof(*result));
    if (result == NULL) {
        libusb_free_device_list(list, 1);
        libusb_exit(context);
        return OST_ERR_NO_MEMORY;
    }
    for (i = 0; i < device_count; ++i) {
        struct libusb_device_descriptor descriptor;

        status = libusb_get_device_descriptor(list[i], &descriptor);
        if (status != 0) {
            free(result);
            libusb_free_device_list(list, 1);
            libusb_exit(context);
            return from_libusb_error(status);
        }
        result[i].vendor_id = descriptor.idVendor;
        result[i].product_id = descriptor.idProduct;
        result[i].bus_number = libusb_get_bus_number(list[i]);
        result[i].device_address = libusb_get_device_address(list[i]);
        status = libusb_get_port_numbers(list[i], result[i].port_numbers, (int)OST_MAX_PORT_DEPTH);
        result[i].port_count = status > 0 ? (uint8_t)status : 0u;
    }
    libusb_free_device_list(list, 1);
    libusb_exit(context);
    *devices = result;
    *count = (size_t)device_count;
    return OST_SUCCESS;
}

static void libusb_free_devices(void *userdata, ost_device_info *devices)
{
    (void)userdata;
    free(devices);
}

static int libusb_open_device(
    void *userdata,
    const ost_device_info *info,
    ost_transport_handle **handle)
{
    struct libusb_handle *opened;
    libusb_context *context = NULL;
    libusb_device_handle *native_handle = NULL;
    libusb_device **list = NULL;
    ssize_t device_count;
    ssize_t i;
    int status = OST_ERR_NOT_FOUND;

    if (ost_hw_guard_check() != OST_SUCCESS) {
        return OST_ERR_UNSUPPORTED;
    }
    (void)userdata;
    *handle = NULL;
    libusb_open_error[0] = '\0';
    opened = calloc(1u, sizeof(*opened));
    if (opened == NULL) {
        return OST_ERR_NO_MEMORY;
    }
    status = libusb_init(&context);
    if (status != 0) {
        free(opened);
        return from_libusb_error(status);
    }
    device_count = libusb_get_device_list(context, &list);
    if (device_count < 0) {
        status = from_libusb_error((int)device_count);
        libusb_exit(context);
        free(opened);
        return status;
    }
    status = OST_ERR_NOT_FOUND;
    for (i = 0; i < device_count; ++i) {
        struct libusb_device_descriptor descriptor;

        if (libusb_get_device_descriptor(list[i], &descriptor) != 0) {
            continue;
        }
        if (descriptor.idVendor == info->vendor_id &&
            descriptor.idProduct == info->product_id &&
            libusb_get_bus_number(list[i]) == info->bus_number &&
            libusb_get_device_address(list[i]) == info->device_address) {
            if (info->port_count > 0u) {
                uint8_t ports[OST_MAX_PORT_DEPTH];
                int depth = libusb_get_port_numbers(list[i], ports, (int)OST_MAX_PORT_DEPTH);

                if (depth != (int)info->port_count || memcmp(ports, info->port_numbers, info->port_count) != 0) {
                    break;
                }
            }
            int open_status = libusb_open(list[i], &native_handle);

            status = open_status == 0 ? OST_SUCCESS : from_libusb_error(open_status);
            if (open_status == LIBUSB_ERROR_ACCESS) {
                set_claim_error(open_status, info);
            }
            break;
        }
    }
    libusb_free_device_list(list, 1);
    if (native_handle == NULL) {
        libusb_exit(context);
        free(opened);
        return status;
    }
    (void)libusb_set_auto_detach_kernel_driver(native_handle, 1);
    status = libusb_claim_interface(native_handle, OST_LIBUSB_INTERFACE);
    if (status != 0) {
        set_claim_error(status, info);
        status = from_libusb_error(status);
        libusb_close(native_handle);
        libusb_exit(context);
        free(opened);
        return status;
    }
    opened->context = context;
    opened->device = native_handle;
    opened->interface_claimed = 1;
    *handle = (ost_transport_handle *)opened;
    return OST_SUCCESS;
}

static void libusb_close_device(void *userdata, ost_transport_handle *handle)
{
    struct libusb_handle *opened = (struct libusb_handle *)handle;

    (void)userdata;
    if (opened->interface_claimed && opened->device != NULL) {
        (void)libusb_release_interface(opened->device, OST_LIBUSB_INTERFACE);
    }
    if (opened->device != NULL) {
        libusb_close(opened->device);
    }
    libusb_exit(opened->context);
    free(opened);
}

static int libusb_control(
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
    struct libusb_handle *opened = (struct libusb_handle *)handle;
    int result;

    (void)userdata;
    result = libusb_control_transfer(
        opened->device,
        request_type,
        request,
        value,
        index,
        data,
        length,
        timeout_ms);
    if (result < 0) {
        return from_libusb_error(result);
    }
    *transferred = result;
    return OST_SUCCESS;
}

static int libusb_bulk(
    void *userdata,
    ost_transport_handle *handle,
    unsigned char endpoint,
    unsigned char *data,
    int length,
    unsigned int timeout_ms,
    int *transferred)
{
    struct libusb_handle *opened = (struct libusb_handle *)handle;
    int result;

    (void)userdata;
    result = libusb_bulk_transfer(
        opened->device,
        endpoint,
        data,
        length,
        transferred,
        timeout_ms);
    return result == 0 ? OST_SUCCESS : from_libusb_error(result);
}

/*
 * Asynchronous image pipe: depth transfers of chunk bytes stay outstanding on one IN endpoint. Completions arrive in
 * submission order, so the slots form a ring: the oldest is consumed, then resubmitted behind the others. The
 * libusb callback only records the result; all other work happens in ost_stream_source calls on the caller's thread,
 * which drive the libusb event loop with a timeout (no busy waiting).
 */
struct stream_pipe;

struct stream_slot {
    struct libusb_transfer *transfer;
    unsigned char *buffer;
    struct stream_pipe *pipe;
    int done;
    int status;
    int length;
};

struct stream_pipe {
    struct libusb_handle *handle;
    unsigned char endpoint;
    size_t chunk;
    unsigned int depth;
    struct stream_slot *slots;
    unsigned int head;
    int held;
    unsigned int in_flight;
};

#ifndef OST_STREAM_CLOSE_DEADLINE_MS
#define OST_STREAM_CLOSE_DEADLINE_MS 5000u
#endif
#define STREAM_CLOSE_SLICE_MS 50u

static double stream_now(void);

static void LIBUSB_CALL stream_completed(struct libusb_transfer *transfer)
{
    struct stream_slot *slot = transfer->user_data;

    slot->done = 1;
    slot->status = transfer->status;
    slot->length = transfer->actual_length;
    --slot->pipe->in_flight;
}

static int stream_submit(struct stream_pipe *pipe, struct stream_slot *slot)
{
    int result;

    slot->done = 0;
    slot->length = 0;
    libusb_fill_bulk_transfer(slot->transfer, pipe->handle->device, pipe->endpoint, slot->buffer, (int)pipe->chunk,
        stream_completed, slot, 0);
    result = libusb_submit_transfer(slot->transfer);
    if (result != 0) {
        slot->done = 1;
        slot->status = LIBUSB_TRANSFER_ERROR;
        return from_libusb_error(result);
    }
    ++pipe->in_flight;
    return OST_SUCCESS;
}

static void stream_pump(struct stream_pipe *pipe, unsigned int timeout_ms)
{
    struct timeval timeout;

    timeout.tv_sec = (long)(timeout_ms / 1000u);
    timeout.tv_usec = (long)((timeout_ms % 1000u) * 1000u);
    (void)libusb_handle_events_timeout_completed(pipe->handle->context, &timeout, NULL);
}

static int stream_close_pipe(void *userdata, void *pipe_pointer, size_t *discarded_bytes)
{
    struct stream_pipe *pipe = pipe_pointer;
    size_t discarded = 0u;
    unsigned int i;
    double deadline;
    int status = OST_SUCCESS;

    (void)userdata;
    if (pipe == NULL) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    for (i = 0u; i < pipe->depth; ++i) {
        if (!pipe->slots[i].done) {
            (void)libusb_cancel_transfer(pipe->slots[i].transfer);
        }
    }
    deadline = stream_now() + (double)OST_STREAM_CLOSE_DEADLINE_MS * 1e-3;
    while (pipe->in_flight != 0u) {
        double remaining = deadline - stream_now();

        if (remaining <= 0.0) {
            break;
        }
        stream_pump(pipe, remaining < (double)STREAM_CLOSE_SLICE_MS * 1e-3
            ? (unsigned int)(remaining * 1e3) + 1u : STREAM_CLOSE_SLICE_MS);
    }
    if (pipe->in_flight != 0u) {
        /* Device close ends libusb ownership when cancellation callbacks cannot arrive. */
        libusb_close(pipe->handle->device);
        pipe->handle->device = NULL;
        pipe->handle->interface_claimed = 0;
        status = OST_ERR_IO;
    }
    for (i = 0u; i < pipe->depth; ++i) {
        struct stream_slot *slot = &pipe->slots[i];

        if ((int)i != pipe->held && (slot->status == LIBUSB_TRANSFER_COMPLETED ||
            slot->status == LIBUSB_TRANSFER_CANCELLED) && slot->length > 0) {
            discarded += (size_t)slot->length;
        }
        libusb_free_transfer(slot->transfer);
        free(slot->buffer);
    }
    free(pipe->slots);
    free(pipe);
    if (discarded_bytes != NULL) {
        *discarded_bytes = discarded;
    }
    return status;
}

static int stream_open_pipe(void *userdata, ost_transport_handle *handle, unsigned char endpoint, size_t chunk_size,
    unsigned int depth, void **pipe_out)
{
    struct libusb_handle *opened = (struct libusb_handle *)handle;
    struct stream_pipe *pipe;
    unsigned int i;
    int status = OST_SUCCESS;

    (void)userdata;
    if (opened == NULL || pipe_out == NULL || chunk_size == 0u || chunk_size > 0x7fffffffu || depth == 0u) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    *pipe_out = NULL;
    pipe = calloc(1u, sizeof(*pipe));
    if (pipe == NULL) {
        return OST_ERR_NO_MEMORY;
    }
    pipe->handle = opened;
    pipe->endpoint = endpoint;
    pipe->chunk = chunk_size;
    pipe->depth = depth;
    pipe->held = -1;
    pipe->slots = calloc(depth, sizeof(*pipe->slots));
    if (pipe->slots == NULL) {
        free(pipe);
        return OST_ERR_NO_MEMORY;
    }
    for (i = 0u; i < depth; ++i) {
        pipe->slots[i].pipe = pipe;
        pipe->slots[i].done = 1;
        pipe->slots[i].transfer = libusb_alloc_transfer(0);
        pipe->slots[i].buffer = malloc(chunk_size);
        if (pipe->slots[i].transfer == NULL || pipe->slots[i].buffer == NULL) {
            status = OST_ERR_NO_MEMORY;
        }
    }
    for (i = 0u; status == OST_SUCCESS && i < depth; ++i) {
        status = stream_submit(pipe, &pipe->slots[i]);
    }
    if (status != OST_SUCCESS) {
        (void)stream_close_pipe(NULL, pipe, NULL);
        return status;
    }
    *pipe_out = pipe;
    return OST_SUCCESS;
}

static double stream_now(void)
{
    return ost_time_seconds();
}

static int stream_wait_pipe(void *userdata, void *pipe_pointer, unsigned int timeout_ms, const unsigned char **data,
    size_t *length, unsigned int *backlog)
{
    struct stream_pipe *pipe = pipe_pointer;
    struct stream_slot *slot;
    double deadline = stream_now() + (double)timeout_ms * 1e-3;
    unsigned int i;

    (void)userdata;
    if (pipe == NULL || data == NULL || length == NULL) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    if (pipe->held >= 0) {
        int status = stream_submit(pipe, &pipe->slots[pipe->held]);

        pipe->held = -1;
        if (status != OST_SUCCESS) {
            return status;
        }
    }
    slot = &pipe->slots[pipe->head];
    while (!slot->done) {
        double remaining = deadline - stream_now();

        if (remaining <= 0.0) {
            return OST_TRANSPORT_ERR_TIMEOUT;
        }
        stream_pump(pipe, (unsigned int)(remaining * 1e3) + 1u);
    }
    if (slot->status != LIBUSB_TRANSFER_COMPLETED) {
        return slot->status == LIBUSB_TRANSFER_NO_DEVICE ? OST_ERR_NOT_FOUND : OST_ERR_IO;
    }
    if (backlog != NULL) {
        *backlog = 0u;
        for (i = 1u; i < pipe->depth; ++i) {
            if (!pipe->slots[(pipe->head + i) % pipe->depth].done) {
                break;
            }
            ++*backlog;
        }
    }
    *data = slot->buffer;
    *length = (size_t)slot->length;
    pipe->held = (int)pipe->head;
    pipe->head = (pipe->head + 1u) % pipe->depth;
    return OST_SUCCESS;
}

const ost_stream_source *ost_libusb_stream_source(void)
{
    static const ost_stream_source source = {
        NULL,
        stream_open_pipe,
        stream_wait_pipe,
        stream_close_pipe,
    };

    return &source;
}

const ost_transport *ost_libusb_transport(void)
{
    static const ost_transport transport = {
        NULL,
        libusb_list_devices,
        libusb_free_devices,
        libusb_open_device,
        libusb_close_device,
        libusb_control,
        libusb_bulk,
    };

    return &transport;
}
