/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#define _POSIX_C_SOURCE 200809L

#include "camera_acquire.h"
#include "camera_settings.h"
#include "camera_startup.h"
#include "camera_stream.h"
#include "openst133.h"
#include "test_check.h"
#include "transport_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_ENDPOINT 0x82u
#define FRAME_BYTES (OST_CAMERA_DEFAULT_WIDTH * OST_CAMERA_DEFAULT_HEIGHT * OST_CAMERA_BYTES_PER_PIXEL)

struct ost_startup_data {
    unsigned char *tables[8];
};

typedef struct fault_device {
    ost_transport transport;
    ost_device *device;
    int close_count;
    int status;
    unsigned int image_reads;
    unsigned int disconnect_at;
    unsigned int partial_at;
    int partial_moved;
    int partial_status;
} fault_device;

static const ost_device_info camera_info = {OST_USB_VENDOR_ID, OST_USB_PRODUCT_ID, 2u, 7u, 0u, {0u}};

static int fault_open(void *userdata, const ost_device_info *info, ost_transport_handle **handle)
{
    (void)userdata;
    (void)info;
    *handle = (ost_transport_handle *)(uintptr_t)1u;
    return OST_SUCCESS;
}

static void fault_close(void *userdata, ost_transport_handle *handle)
{
    fault_device *fault = userdata;

    (void)handle;
    ++fault->close_count;
}

static int fault_control(void *userdata, ost_transport_handle *handle, uint8_t request_type, uint8_t request,
    uint16_t value, uint16_t index, unsigned char *data, uint16_t length, unsigned int timeout_ms, int *transferred)
{
    (void)handle;
    (void)request_type;
    (void)request;
    (void)value;
    (void)index;
    (void)data;
    (void)timeout_ms;
    *transferred = length;
    return ((fault_device *)userdata)->status;
}

static int fault_bulk(void *userdata, ost_transport_handle *handle, unsigned char endpoint, unsigned char *data,
    int length, unsigned int timeout_ms, int *transferred)
{
    fault_device *fault = userdata;

    (void)handle;
    (void)timeout_ms;
    if (fault->status != OST_SUCCESS) {
        return fault->status;
    }
    if ((endpoint & 0x80u) == 0u) {
        *transferred = length;
        return OST_SUCCESS;
    }
    if (endpoint == 0x86u) {
        memset(data, 0x9b, (size_t)length);
        *transferred = length;
        return OST_SUCCESS;
    }
    ++fault->image_reads;
    if (fault->partial_at != 0u && fault->image_reads == fault->partial_at) {
        memset(data, 0xa5, (size_t)fault->partial_moved);
        *transferred = fault->partial_moved;
        return fault->partial_status;
    }
    if (fault->disconnect_at != 0u && fault->image_reads == fault->disconnect_at) {
        *transferred = 0;
        return OST_ERR_NOT_FOUND;
    }
    memset(data, (int)(fault->image_reads & 0xffu), (size_t)length);
    *transferred = length;
    return OST_SUCCESS;
}

static void fault_device_open(fault_device *fault)
{
    memset(fault, 0, sizeof(*fault));
    fault->transport.userdata = fault;
    fault->transport.open = fault_open;
    fault->transport.close = fault_close;
    fault->transport.control_transfer = fault_control;
    fault->transport.bulk_transfer = fault_bulk;
    TEST_CHECK(ost_open(&fault->transport, &camera_info, &fault->device) == OST_SUCCESS);
}

static const ost_settings_plan *default_plan(void)
{
    static ost_settings_plan plan;
    ost_settings settings;
    char error[256];

    memset(&settings, 0, sizeof(settings));
    settings.exposure_ms = 1u;
    TEST_CHECK(ost_settings_plan_make(&settings, OST_SETTINGS_TABLE_1_MHZ, &plan, error, sizeof(error)) ==
        OST_SUCCESS);
    return &plan;
}

static void run_acquisition(fault_device *fault, int injected, int expected, unsigned int disconnect_at,
    size_t *received_out)
{
    ost_acquire_options options;
    ost_startup_data data;
    unsigned char table_one[1368] = {0};
    unsigned char table_two[512] = {0};
    unsigned char *frame = calloc(FRAME_BYTES, 1u);
    size_t received = 0u;
    char error[256];
    int result;

    TEST_CHECK(frame != NULL);
    memset(&options, 0, sizeof(options));
    memset(&data, 0, sizeof(data));
    data.tables[1] = table_one;
    data.tables[2] = table_two;
    options.data = &data;
    options.exposure_ms = 1u;
    options.plan = default_plan();
    fault->status = injected;
    fault->image_reads = 0u;
    fault->disconnect_at = disconnect_at;
    result = ost_acquire_run(fault->device, &options, frame, FRAME_BYTES, &received, error, sizeof(error));
    if (result != expected) {
        (void)fprintf(stderr, "acquisition returned %d (%s), expected %d; error: %s\n", result,
            ost_status_text(result), expected, error);
    }
    TEST_CHECK(result == expected);
    if (received_out != NULL) {
        *received_out = received;
    }
    fault->status = injected;
    free(frame);
}

static void test_public_timeout_and_disconnect(void)
{
    fault_device fault;
    unsigned char response[8] = {0};
    int moved = -1;
    size_t received = 0u;

    fault_device_open(&fault);
    fault.status = OST_TRANSPORT_ERR_TIMEOUT;
    TEST_CHECK(ost_bulk_transfer(fault.device, 0x86u, response, 2, 50u, &moved) == OST_ERR_TIMEOUT);
    TEST_CHECK(ost_control_transfer(fault.device, 0x40u, 0xf0u, 0u, 0u, response, sizeof(response), 50u, &moved) ==
        OST_ERR_TIMEOUT);
    TEST_CHECK(strcmp(ost_status_text(OST_ERR_TIMEOUT), "timeout") == 0);
    fault.status = OST_ERR_NOT_FOUND;
    TEST_CHECK(ost_bulk_transfer(fault.device, 0x86u, response, 2, 50u, &moved) == OST_ERR_NOT_FOUND);
    run_acquisition(&fault, OST_TRANSPORT_ERR_TIMEOUT, OST_ERR_TIMEOUT, 0u, NULL);
    fault.status = OST_SUCCESS;
    run_acquisition(&fault, OST_SUCCESS, OST_ERR_NOT_FOUND, 1u, &received);
    TEST_CHECK(received == 0u);
    fault.partial_at = 2u;
    fault.partial_moved = 128;
    fault.partial_status = OST_TRANSPORT_ERR_TIMEOUT;
    received = 0u;
    run_acquisition(&fault, OST_SUCCESS, OST_ERR_TIMEOUT, 0u, &received);
    TEST_CHECK(received == 19968u + 128u);
    ost_close(fault.device);
    ost_close(NULL);
    ost_camera_close(NULL);
    TEST_CHECK(fault.close_count == 1);
    (void)fprintf(stdout, "public transfer timeout, acquisition unplug, partial-read accounting, single close: ok\n");
}

typedef struct source {
    ost_stream_source interface;
    int fault_status;
    unsigned int waits;
    unsigned int complete_frames_before_fault;
    size_t offset;
    size_t frame_size;
    unsigned char data[4u * OST_ACQUIRE_FRAME_BYTES];
    int open_count;
    int close_count;
    size_t discarded;
} source;

static int source_open(void *userdata, ost_transport_handle *handle, unsigned char endpoint, size_t chunk_size,
    unsigned int depth, void **pipe)
{
    source *source = userdata;

    (void)handle;
    (void)chunk_size;
    (void)depth;
    TEST_CHECK(endpoint == IMAGE_ENDPOINT);
    ++source->open_count;
    *pipe = source;
    return OST_SUCCESS;
}

static int source_wait(void *userdata, void *pipe, unsigned int timeout_ms, const unsigned char **data,
    size_t *length, unsigned int *backlog)
{
    source *source = userdata;

    (void)pipe;
    (void)timeout_ms;
    ++source->waits;
    TEST_CHECK(source->waits < 100u);
    *backlog = 0u;
    if (source->offset >= (size_t)source->complete_frames_before_fault * source->frame_size) {
        return source->fault_status;
    }
    *data = source->data + source->offset;
    *length = sizeof(source->data) - source->offset;
    if (*length > 16384u) {
        *length = 16384u;
    }
    source->offset += *length;
    return OST_SUCCESS;
}

static int source_close(void *userdata, void *pipe, size_t *discarded_bytes)
{
    source *source = userdata;

    (void)pipe;
    ++source->close_count;
    *discarded_bytes = source->discarded;
    return OST_SUCCESS;
}

static void source_init(source *source, int status, unsigned int complete_frames, size_t frame_size)
{
    size_t i;

    memset(source, 0, sizeof(*source));
    TEST_CHECK(frame_size <= sizeof(source->data) && complete_frames <= 4u);
    for (i = 0u; i < sizeof(source->data); ++i) {
        source->data[i] = (unsigned char)((i * 7u + (i >> 8u)) & 0xffu);
    }
    source->fault_status = status;
    source->complete_frames_before_fault = complete_frames;
    source->frame_size = frame_size;
    source->interface.userdata = source;
    source->interface.open = source_open;
    source->interface.wait = source_wait;
    source->interface.close = source_close;
}

static unsigned int delivered_frames;

static int count_frame(void *userdata, uint32_t index, const void *frame, size_t size)
{
    (void)frame;
    TEST_CHECK(userdata == &delivered_frames);
    TEST_CHECK(index == delivered_frames);
    TEST_CHECK(size == FRAME_BYTES);
    ++delivered_frames;
    return OST_SUCCESS;
}

static void run_stream_fault(int injected, int expected, unsigned int complete_frames)
{
    fault_device fault;
    source source;
    ost_acquire_options options;
    ost_stream_run_options stream_options;
    ost_startup_data data;
    unsigned char table_one[1368] = {0};
    unsigned char table_two[512] = {0};
    ost_stream_stats stats;
    char error[256];
    int result;

    fault_device_open(&fault);
    source_init(&source, injected, complete_frames, FRAME_BYTES);
    memset(&options, 0, sizeof(options));
    memset(&stream_options, 0, sizeof(stream_options));
    memset(&data, 0, sizeof(data));
    data.tables[1] = table_one;
    data.tables[2] = table_two;
    options.data = &data;
    options.exposure_ms = 1u;
    options.plan = default_plan();
    stream_options.source = &source.interface;
    stream_options.chunk_size = 16384u;
    stream_options.depth = 4u;
    stream_options.idle_timeout_ms = 50u;
    delivered_frames = 0u;
    result = ost_camera_stream_run(fault.device, &options, &stream_options, 4u, count_frame, &delivered_frames,
        &stats, error, sizeof(error));
    if (result != expected) {
        (void)fprintf(stderr, "stream returned %d (%s), expected %d; error: %s\n", result, ost_status_text(result),
            expected, error);
    }
    TEST_CHECK(result == expected);
    TEST_CHECK(delivered_frames == complete_frames);
    TEST_CHECK(stats.frames_delivered == complete_frames);
    TEST_CHECK(source.open_count == 1 && source.close_count == 1);
    TEST_CHECK(stats.bytes_received >= (size_t)complete_frames * FRAME_BYTES);
    TEST_CHECK(stats.bytes_received - (size_t)complete_frames * FRAME_BYTES <= 16384u);
    TEST_CHECK(stats.partial_bytes_discarded == stats.bytes_received - (size_t)complete_frames * FRAME_BYTES);
    ost_close(fault.device);
    TEST_CHECK(fault.close_count == 1);
}

static void test_stream_unplug_and_timeout(void)
{
    run_stream_fault(OST_ERR_NOT_FOUND, OST_ERR_NOT_FOUND, 2u);
    run_stream_fault(OST_TRANSPORT_ERR_TIMEOUT, OST_ERR_TIMEOUT, 1u);
    run_stream_fault(OST_ERR_TIMEOUT, OST_ERR_TIMEOUT, 1u);
    (void)fprintf(stdout, "stream unplug after two frames, internal and public idle timeouts: ok\n");
}

int main(void)
{
    test_public_timeout_and_disconnect();
    test_stream_unplug_and_timeout();
    return EXIT_SUCCESS;
}
