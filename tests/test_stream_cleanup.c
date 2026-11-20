/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#include "openst133.h"
#include "test_check.h"
#include "transport_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define OST_STREAM_CLOSE_DEADLINE_MS 20u
#include "../src/libusb_transport.c"
#undef OST_STREAM_CLOSE_DEADLINE_MS

static unsigned int close_calls;
static unsigned int release_calls;
static unsigned int exit_calls;
static unsigned int cancel_calls;
static unsigned int event_calls;
static unsigned int free_calls;
static unsigned char native_handle_storage[16];
static unsigned char native_context_storage[16];

int LIBUSB_CALL libusb_init(libusb_context **context)
{
    *context = (libusb_context *)(void *)native_context_storage;
    return 0;
}

void LIBUSB_CALL libusb_exit(libusb_context *context)
{
    (void)context;
    ++exit_calls;
}

ssize_t LIBUSB_CALL libusb_get_device_list(libusb_context *context, libusb_device ***list)
{
    (void)context;
    *list = NULL;
    return 0;
}

void LIBUSB_CALL libusb_free_device_list(libusb_device **list, int unref_devices)
{
    (void)list;
    (void)unref_devices;
}

int LIBUSB_CALL libusb_get_device_descriptor(libusb_device *device, struct libusb_device_descriptor *descriptor)
{
    (void)device;
    (void)descriptor;
    return LIBUSB_ERROR_NOT_FOUND;
}

uint8_t LIBUSB_CALL libusb_get_bus_number(libusb_device *device)
{
    (void)device;
    return 0u;
}

uint8_t LIBUSB_CALL libusb_get_device_address(libusb_device *device)
{
    (void)device;
    return 0u;
}

int LIBUSB_CALL libusb_open(libusb_device *device, libusb_device_handle **handle)
{
    (void)device;
    *handle = (libusb_device_handle *)(void *)native_handle_storage;
    return 0;
}

void LIBUSB_CALL libusb_close(libusb_device_handle *handle)
{
    TEST_CHECK(handle == (libusb_device_handle *)(void *)native_handle_storage);
    ++close_calls;
}

int LIBUSB_CALL libusb_set_auto_detach_kernel_driver(libusb_device_handle *handle, int enable)
{
    (void)handle;
    (void)enable;
    return 0;
}

int LIBUSB_CALL libusb_claim_interface(libusb_device_handle *handle, int interface_number)
{
    (void)handle;
    (void)interface_number;
    return 0;
}

int LIBUSB_CALL libusb_release_interface(libusb_device_handle *handle, int interface_number)
{
    (void)handle;
    (void)interface_number;
    ++release_calls;
    return 0;
}

int LIBUSB_CALL libusb_control_transfer(libusb_device_handle *handle, uint8_t request_type, uint8_t request,
    uint16_t value, uint16_t index, unsigned char *data, uint16_t length, unsigned int timeout)
{
    (void)handle;
    (void)request_type;
    (void)request;
    (void)value;
    (void)index;
    (void)data;
    (void)length;
    (void)timeout;
    return 0;
}

int LIBUSB_CALL libusb_bulk_transfer(libusb_device_handle *handle, unsigned char endpoint, unsigned char *data,
    int length, int *actual_length, unsigned int timeout)
{
    (void)handle;
    (void)endpoint;
    (void)data;
    (void)length;
    (void)actual_length;
    (void)timeout;
    return 0;
}

struct libusb_transfer *LIBUSB_CALL libusb_alloc_transfer(int iso_packets)
{
    (void)iso_packets;
    return calloc(1u, sizeof(struct libusb_transfer));
}

int LIBUSB_CALL libusb_submit_transfer(struct libusb_transfer *transfer)
{
    (void)transfer;
    return 0;
}

int LIBUSB_CALL libusb_cancel_transfer(struct libusb_transfer *transfer)
{
    (void)transfer;
    ++cancel_calls;
    return 0;
}

void LIBUSB_CALL libusb_free_transfer(struct libusb_transfer *transfer)
{
    TEST_CHECK(close_calls == 1u);
    ++free_calls;
    free(transfer);
}

int LIBUSB_CALL libusb_handle_events_timeout_completed(libusb_context *context, struct timeval *timeout, int *completed)
{
    const struct timespec delay = {0, 1000000L};

    (void)context;
    (void)completed;
    TEST_CHECK(timeout != NULL);
    TEST_CHECK(timeout->tv_sec == 0);
    TEST_CHECK(timeout->tv_usec > 0 && timeout->tv_usec <= 50000L);
    ++event_calls;
    TEST_CHECK(nanosleep(&delay, NULL) == 0);
    return 0;
}

int main(void)
{
    const ost_stream_source *source = ost_libusb_stream_source();
    struct libusb_handle *opened;
    void *pipe = NULL;
    size_t discarded = 123u;
    double start;
    double elapsed;
    int status;

    opened = calloc(1u, sizeof(*opened));
    TEST_CHECK(opened != NULL);
    opened->context = (libusb_context *)(void *)native_context_storage;
    opened->device = (libusb_device_handle *)(void *)native_handle_storage;
    opened->interface_claimed = 1;
    status = source->open(NULL, (ost_transport_handle *)opened, 0x82u, 64u, 3u, &pipe);
    TEST_CHECK(status == OST_SUCCESS);
    TEST_CHECK(pipe != NULL);

    start = ost_time_seconds();
    status = source->close(NULL, pipe, &discarded);
    elapsed = ost_time_seconds() - start;
    TEST_CHECK(status == OST_ERR_IO);
    TEST_CHECK(discarded == 0u);
    TEST_CHECK(cancel_calls == 3u);
    TEST_CHECK(event_calls > 0u);
    TEST_CHECK(elapsed >= 0.015);
    TEST_CHECK(elapsed < 1.0);
    TEST_CHECK(close_calls == 1u);
    TEST_CHECK(free_calls == 3u);
    TEST_CHECK(opened->device == NULL);
    TEST_CHECK(opened->interface_claimed == 0);

    libusb_close_device(NULL, (ost_transport_handle *)opened);
    TEST_CHECK(close_calls == 1u);
    TEST_CHECK(release_calls == 0u);
    TEST_CHECK(exit_calls == 1u);
    (void)fprintf(stdout, "stuck stream close: 3 cancellations, %u event waits, %.3f s, 3 frees, one device close\n",
        event_calls, elapsed);
    return 0;
}
