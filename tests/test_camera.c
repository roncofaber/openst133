/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#include "openst133.h"
#include "camera_startup.h"
#include "pcap_usbmon.h"
#include "protocol.h"
#include "test_check.h"
#include "transport_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINKTYPE_USB_LINUX 189u
#define CHUNK 0x8000u
#define FRAME_BYTES (OST_CAMERA_DEFAULT_WIDTH * OST_CAMERA_DEFAULT_HEIGHT * OST_CAMERA_BYTES_PER_PIXEL)

static const unsigned char device_descriptor[] = {
    0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40,
    0xd7, 0x0b, 0x10, 0xa0, 0x00, 0x01, 0x01, 0x02, 0x03, 0x01,
};

/* Fixture: expected-transfer script written as a usbmon capture for the replay transport. */
struct trace_event {
    uint8_t event;
    uint8_t transfer_type;
    uint8_t endpoint;
    uint8_t request_type;
    uint8_t request;
    uint16_t setup_value;
    uint16_t setup_length;
    int32_t status;
    uint32_t length;
    unsigned char *data;
    uint32_t data_size;
};

struct script {
    struct trace_event *events;
    size_t count;
    size_t capacity;
};

static struct trace_event *script_next(struct script *script)
{
    if (script->count == script->capacity) {
        script->capacity = script->capacity == 0u ? 64u : script->capacity * 2u;
        script->events = realloc(script->events, script->capacity * sizeof(*script->events));
        TEST_CHECK(script->events != NULL);
    }
    memset(&script->events[script->count], 0, sizeof(script->events[0]));
    return &script->events[script->count++];
}

static unsigned char *copy_bytes(const unsigned char *data, uint32_t size)
{
    unsigned char *copy = malloc(size == 0u ? 1u : size);

    TEST_CHECK(copy != NULL);
    if (size != 0u) {
        memcpy(copy, data, size);
    }
    return copy;
}

static void script_init(struct script *script)
{
    struct trace_event *event;

    memset(script, 0, sizeof(*script));
    event = script_next(script);
    event->event = 'S';
    event->transfer_type = 2u;
    event->endpoint = 0x80u;
    event->request_type = 0x80u;
    event->request = 6u;
    event->setup_value = 0x0100u;
    event->setup_length = 64u;
    event->length = 64u;
    event = script_next(script);
    event->event = 'C';
    event->transfer_type = 2u;
    event->endpoint = 0x80u;
    event->request_type = 0x80u;
    event->request = 6u;
    event->setup_value = 0x0100u;
    event->setup_length = 64u;
    event->length = sizeof(device_descriptor);
    event->data = copy_bytes(device_descriptor, sizeof(device_descriptor));
    event->data_size = sizeof(device_descriptor);
}

static void script_free(struct script *script)
{
    size_t i;

    for (i = 0u; i < script->count; ++i) {
        free(script->events[i].data);
    }
    free(script->events);
    memset(script, 0, sizeof(*script));
}

static void add_pair(struct script *script, const struct trace_event *submission, int32_t status,
    uint32_t completed, const unsigned char *data, uint32_t size)
{
    struct trace_event *event = script_next(script);

    *event = *submission;
    event->data = submission->data_size == 0u ? NULL : copy_bytes(submission->data, submission->data_size);
    event = script_next(script);
    *event = *submission;
    event->event = 'C';
    event->status = status;
    event->length = completed;
    event->data = size == 0u ? NULL : copy_bytes(data, size);
    event->data_size = size;
}

static void add_bulk_out(struct script *script, uint8_t endpoint, const unsigned char *data, uint32_t size)
{
    struct trace_event submission;

    memset(&submission, 0, sizeof(submission));
    submission.event = 'S';
    submission.transfer_type = 3u;
    submission.endpoint = endpoint;
    submission.length = size;
    submission.data = (unsigned char *)(uintptr_t)data;
    submission.data_size = size;
    add_pair(script, &submission, 0, size, NULL, 0u);
}

static void add_bulk_in(struct script *script, uint8_t endpoint, uint32_t requested,
    const unsigned char *data, uint32_t size)
{
    struct trace_event submission;

    memset(&submission, 0, sizeof(submission));
    submission.event = 'S';
    submission.transfer_type = 3u;
    submission.endpoint = endpoint;
    submission.length = requested;
    add_pair(script, &submission, 0, size, data, size);
}

static void add_vendor_out(struct script *script, uint8_t request, uint16_t value)
{
    static const unsigned char zeros[8] = {0};
    struct trace_event submission;

    memset(&submission, 0, sizeof(submission));
    submission.event = 'S';
    submission.transfer_type = 2u;
    submission.request_type = 0x40u;
    submission.request = request;
    submission.setup_value = value;
    submission.setup_length = 8u;
    submission.length = 8u;
    submission.data = (unsigned char *)(uintptr_t)zeros;
    submission.data_size = 8u;
    add_pair(script, &submission, 0, 8u, NULL, 0u);
}

static void write_le(unsigned char *out, uint64_t value, size_t width)
{
    size_t i;

    for (i = 0u; i < width; ++i) {
        out[i] = (unsigned char)((value >> (8u * i)) & 0xffu);
    }
}

static void write_script(const char *path, const struct script *script)
{
    FILE *file = fopen(path, "wb");
    unsigned char header_block[24];
    size_t i;

    TEST_CHECK(file != NULL);
    write_le(header_block + 0, 0xa1b2c3d4u, 4u);
    write_le(header_block + 4, 2u, 2u);
    write_le(header_block + 6, 4u, 2u);
    write_le(header_block + 8, 0u, 4u);
    write_le(header_block + 12, 0u, 4u);
    write_le(header_block + 16, 65535u, 4u);
    write_le(header_block + 20, LINKTYPE_USB_LINUX, 4u);
    TEST_CHECK(fwrite(header_block, 1u, sizeof(header_block), file) == sizeof(header_block));
    for (i = 0u; i < script->count; ++i) {
        const struct trace_event *event = &script->events[i];
        unsigned char record[16];
        unsigned char header[48] = {0};
        uint32_t packet_size = 48u + event->data_size;

        write_le(record + 0, 100u, 4u);
        write_le(record + 4, i + 1u, 4u);
        write_le(record + 8, packet_size, 4u);
        write_le(record + 12, packet_size, 4u);
        TEST_CHECK(fwrite(record, 1u, sizeof(record), file) == sizeof(record));
        write_le(header + 0, 0x0102030405060700ULL + i / 2u, 8u);
        header[8] = event->event;
        header[9] = event->transfer_type;
        header[10] = event->endpoint;
        header[11] = 7u;
        write_le(header + 12, 2u, 2u);
        header[14] = event->event == 'S' && event->transfer_type == 2u ? 0u : 0xffu;
        header[15] = event->data_size == 0u ? 0xffu : 0u;
        write_le(header + 16, 100u, 8u);
        write_le(header + 24, i + 1u, 4u);
        write_le(header + 28, (uint32_t)event->status, 4u);
        write_le(header + 32, event->length, 4u);
        write_le(header + 36, event->data_size, 4u);
        header[40] = event->request_type;
        header[41] = event->request;
        write_le(header + 42, event->setup_value, 2u);
        write_le(header + 46, event->setup_length, 2u);
        TEST_CHECK(fwrite(header, 1u, sizeof(header), file) == sizeof(header));
        if (event->data_size != 0u) {
            TEST_CHECK(fwrite(event->data, 1u, event->data_size, file) == event->data_size);
        }
    }
    TEST_CHECK(fclose(file) == 0);
}

/* The generic transfer setup for a frame, as the protocol layer writes it. */
static void add_frame_setup(struct script *script, uint32_t frame_bytes)
{
    static const unsigned char r1[] = {0x01, 0x14, 0x00, 0x02, 0x00, 0x01};
    static const unsigned char r2[] = {0x01, 0x10, 0x00, 0x02, 0x00, 0x00};
    static const unsigned char r3[] = {0x01, 0x12, 0x00, 0x02, 0x00, 0x00};
    static const unsigned char r4[] = {0x01, 0x14, 0x00, 0x02, 0x01, 0x00};
    static const unsigned char r5[] = {0x01, 0x16, 0x00, 0x02, 0x01, 0x00};
    unsigned char size_low[6];
    unsigned char size_high[6];
    unsigned char remainder[6];

    memcpy(size_low, r2, sizeof(size_low));
    memcpy(size_high, r3, sizeof(size_high));
    memcpy(remainder, r5, sizeof(remainder));
    size_low[4] = (unsigned char)(frame_bytes & 0xffu);
    size_low[5] = (unsigned char)((frame_bytes >> 8u) & 0xffu);
    size_high[4] = (unsigned char)((frame_bytes >> 16u) & 0xffu);
    size_high[5] = (unsigned char)((frame_bytes >> 24u) & 0xffu);
    remainder[4] = frame_bytes < OST_PROTO_MIN_CHUNK_DERIVATION_SIZE ? 0u : frame_bytes % CHUNK != 0u;
    add_bulk_out(script, 0x08u, r1, 6u);
    add_bulk_out(script, 0x08u, size_low, 6u);
    add_bulk_out(script, 0x08u, size_high, 6u);
    add_bulk_out(script, 0x08u, r4, 6u);
    add_vendor_out(script, 0xf2u,
        frame_bytes < OST_PROTO_MIN_CHUNK_DERIVATION_SIZE ? (uint16_t)frame_bytes : CHUNK);
    add_bulk_out(script, 0x08u, remainder, 6u);
}

static unsigned char pattern(size_t offset)
{
    return (unsigned char)((offset * 7u + (offset >> 8u)) & 0xffu);
}

static void add_frame_chunks(struct script *script, size_t limit_bytes, size_t short_after)
{
    size_t done = 0u;
    unsigned char *chunk = malloc(CHUNK);
    size_t i;

    TEST_CHECK(chunk != NULL);
    while (done < limit_bytes) {
        size_t want = limit_bytes - done < CHUNK ? limit_bytes - done : CHUNK;
        size_t give = short_after != 0u && done == 0u ? short_after : want;

        for (i = 0u; i < give; ++i) {
            chunk[i] = pattern(done + i);
        }
        add_bulk_in(script, 0x82u, (uint32_t)want, chunk, (uint32_t)give);
        done += give;
        if (give != want) {
            add_bulk_in(script, 0x82u, (uint32_t)(want - give), NULL, 0u);
            break;
        }
    }
    free(chunk);
}

static const ost_device_info camera_info = {OST_USB_VENDOR_ID, OST_USB_PRODUCT_ID, 2u, 7u, 0u, {0u}};

struct replay_fixture {
    struct script script;
    ost_replay_transport *replay;
    ost_camera *camera;
};

static void fixture_open(struct replay_fixture *fixture, const char *directory, const char *name)
{
    char path[1024];
    char error[256];

    TEST_CHECK(snprintf(path, sizeof(path), "%s/%s", directory, name) > 0);
    write_script(path, &fixture->script);
    TEST_CHECK(ost_replay_transport_create(path, &fixture->replay, error, sizeof(error)) == OST_SUCCESS);
    TEST_CHECK(ost_camera_open(ost_replay_transport_interface(fixture->replay), &camera_info, &fixture->camera) ==
        OST_SUCCESS);
}

static void fixture_close(struct replay_fixture *fixture)
{
    ost_camera_close(fixture->camera);
    ost_replay_transport_destroy(fixture->replay);
    script_free(&fixture->script);
}

static void test_replay_frame(const char *directory)
{
    struct replay_fixture fixture;
    unsigned char *frame = malloc(FRAME_BYTES);
    size_t acquired = 0u;
    size_t i;
    uint32_t exposure = 99u;
    ost_camera_identity identity;
    ost_sensor_geometry geometry;
    double celsius = 0.0;

    TEST_CHECK(frame != NULL);
    script_init(&fixture.script);
    add_frame_setup(&fixture.script, FRAME_BYTES);
    add_frame_chunks(&fixture.script, FRAME_BYTES, 0u);
    fixture_open(&fixture, directory, "frame.pcap");
    TEST_CHECK(ost_camera_get_identity(fixture.camera, &identity) == OST_ERR_UNSUPPORTED);
    TEST_CHECK(ost_camera_get_geometry(fixture.camera, &geometry) == OST_SUCCESS);
    TEST_CHECK(geometry.width == 1340u && geometry.height == 100u && geometry.reported == 0);
    TEST_CHECK(ost_camera_get_exposure(fixture.camera, &exposure) == OST_SUCCESS && exposure == 0u);
    TEST_CHECK(ost_camera_set_exposure(fixture.camera, 0u) == OST_ERR_INVALID_ARGUMENT);
    TEST_CHECK(ost_camera_set_exposure(fixture.camera, 250u) == OST_SUCCESS);
    TEST_CHECK(ost_camera_get_exposure(fixture.camera, &exposure) == OST_SUCCESS && exposure == 250u);
    TEST_CHECK(ost_camera_get_temperature(fixture.camera, &celsius) == OST_ERR_UNSUPPORTED);
    TEST_CHECK(strstr(ost_camera_last_error(fixture.camera), "not supported yet") != NULL);
    TEST_CHECK(ost_camera_set_temperature(fixture.camera, -90.0) == OST_ERR_UNSUPPORTED);
    TEST_CHECK(ost_camera_acquire(fixture.camera, frame, FRAME_BYTES - 1u, &acquired) == OST_ERR_BUFFER_TOO_SMALL);
    TEST_CHECK(acquired == 0u);
    TEST_CHECK(ost_camera_acquire(NULL, frame, FRAME_BYTES, &acquired) == OST_ERR_INVALID_ARGUMENT);
    TEST_CHECK(ost_camera_acquire(fixture.camera, NULL, FRAME_BYTES, &acquired) == OST_ERR_INVALID_ARGUMENT);
    TEST_CHECK(ost_camera_acquire(fixture.camera, frame, FRAME_BYTES, &acquired) == OST_SUCCESS);
    TEST_CHECK(acquired == FRAME_BYTES);
    for (i = 0u; i < FRAME_BYTES; ++i) {
        TEST_CHECK(frame[i] == pattern(i));
    }
    TEST_CHECK(ost_camera_abort(fixture.camera) == OST_SUCCESS);
    TEST_CHECK(ost_camera_query_support(fixture.camera, OST_CAMERA_OP_ACQUIRE) == OST_SUCCESS);
    TEST_CHECK(ost_camera_query_support(fixture.camera, OST_CAMERA_OP_GET_TEMPERATURE) == OST_ERR_UNSUPPORTED);
    fixture_close(&fixture);
    free(frame);
}

static void test_replay_short_and_exhausted(const char *directory)
{
    struct replay_fixture fixture;
    unsigned char *frame = malloc(FRAME_BYTES);
    size_t acquired = 0u;

    TEST_CHECK(frame != NULL);
    script_init(&fixture.script);
    add_frame_setup(&fixture.script, FRAME_BYTES);
    add_frame_chunks(&fixture.script, FRAME_BYTES, 100u);
    fixture_open(&fixture, directory, "short.pcap");
    TEST_CHECK(ost_camera_acquire(fixture.camera, frame, FRAME_BYTES, &acquired) == OST_ERR_IO);
    TEST_CHECK(acquired == 100u);
    TEST_CHECK(strstr(ost_camera_last_error(fixture.camera), "acquisition failed") != NULL);
    TEST_CHECK(ost_camera_acquire(fixture.camera, frame, FRAME_BYTES, &acquired) == OST_ERR_IO);
    TEST_CHECK(acquired == 0u);
    fixture_close(&fixture);
    free(frame);
}

static void test_readout_replay(const char *directory)
{
    static const uint32_t fvb_bytes = 1340u * OST_CAMERA_BYTES_PER_PIXEL;
    static const uint32_t roi_bytes = 30u * 4u * OST_CAMERA_BYTES_PER_PIXEL;
    const ost_camera_readout invalid[] = {
        {0u, 0u, 0u, 100u, 1u, 1u},
        {0u, 0u, 1340u, 0u, 1u, 1u},
        {0u, 0u, 1340u, 100u, 0u, 1u},
        {0u,1u, 1340u, 100u, 1u, 0u},
        {1340u, 0u, 1u, 1u, 1u, 1u},
        {1u, 0u, 1340u, 100u, 1u, 1u},
        {0u, 99u, 1340u, 2u, 1u, 1u},
        {0u, 0u, 100u, 100u, 3u, 1u},
        {0u, 0u, 1340u, 98u, 1u, 3u},
        {0xffffffffu, 0u, 2u, 100u, 1u, 1u},
    };
    ost_camera_readout readout;
    ost_camera_readout expected;
    struct replay_fixture fixture;
    unsigned char *frame = malloc(FRAME_BYTES);
    size_t acquired = 0u;
    size_t i;

    TEST_CHECK(frame != NULL);
    script_init(&fixture.script);
    add_frame_setup(&fixture.script, fvb_bytes);
    add_frame_chunks(&fixture.script, fvb_bytes, 0u);
    add_frame_setup(&fixture.script, roi_bytes);
    add_frame_chunks(&fixture.script, roi_bytes, 0u);
    fixture_open(&fixture, directory, "readout.pcap");
    TEST_CHECK(ost_camera_get_readout(fixture.camera, &readout) == OST_SUCCESS);
    TEST_CHECK(readout.width == 1340u && readout.height == 100u);
    TEST_CHECK(readout.x_binning == 1u && readout.y_binning == 1u);
    TEST_CHECK(ost_camera_get_readout(fixture.camera, NULL) == OST_ERR_INVALID_ARGUMENT);
    expected = readout;
    for (i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        TEST_CHECK(ost_camera_set_readout(fixture.camera, &invalid[i]) == OST_ERR_INVALID_ARGUMENT);
    }
    TEST_CHECK(ost_camera_set_readout(fixture.camera, NULL) == OST_ERR_INVALID_ARGUMENT);
    TEST_CHECK(ost_camera_get_readout(fixture.camera, &readout) == OST_SUCCESS);
    TEST_CHECK(memcmp(&readout, &expected, sizeof(readout)) == 0);
    TEST_CHECK(ost_camera_query_support(fixture.camera, OST_CAMERA_OP_SET_READOUT) == OST_SUCCESS);
    TEST_CHECK(ost_camera_set_full_vertical_binning(fixture.camera) == OST_SUCCESS);
    TEST_CHECK(ost_camera_get_readout(fixture.camera, &readout) == OST_SUCCESS);
    TEST_CHECK(readout.width == 1340u && readout.height == 100u);
    TEST_CHECK(readout.x_binning == 1u && readout.y_binning == 100u);
    TEST_CHECK(ost_camera_acquire(fixture.camera, frame, fvb_bytes - 1u, &acquired) ==
        OST_ERR_BUFFER_TOO_SMALL);
    TEST_CHECK(ost_camera_acquire(fixture.camera, frame, fvb_bytes, &acquired) == OST_SUCCESS);
    TEST_CHECK(acquired == fvb_bytes);
    readout.x = 10u;
    readout.y =4u;
    readout.width = 120u;
    readout.height = 20u;
    readout.x_binning = 4u;
    readout.y_binning = 5u;
    TEST_CHECK(ost_camera_set_readout(fixture.camera, &readout) == OST_SUCCESS);
    TEST_CHECK(ost_camera_acquire(fixture.camera, frame, roi_bytes, &acquired) == OST_SUCCESS);
    TEST_CHECK(acquired == roi_bytes);
    for (i = 0u; i < acquired; ++i) {
        TEST_CHECK(frame[i] == pattern(i));
    }
    fixture_close(&fixture);
    free(frame);
}

/* Mock transport: records every operation so a test can assert what was sent. */
struct mock {
    unsigned int opens;
    unsigned int closes;
    unsigned int controls;
    unsigned int bulk_out;
    unsigned int bulk_in;
    unsigned int abort_on_image_read;
    ost_camera *camera;
};

static int mock_list(void *userdata, ost_device_info **devices, size_t *count)
{
    (void)userdata;
    *devices = NULL;
    *count = 0u;
    return OST_SUCCESS;
}

static void mock_free(void *userdata, ost_device_info *devices)
{
    (void)userdata;
    free(devices);
}

static int mock_open(void *userdata, const ost_device_info *info, ost_transport_handle **handle)
{
    struct mock *mock = userdata;

    (void)info;
    ++mock->opens;
    *handle = (ost_transport_handle *)mock;
    return OST_SUCCESS;
}

static void mock_close(void *userdata, ost_transport_handle *handle)
{
    struct mock *mock = userdata;

    (void)handle;
    ++mock->closes;
}

static int mock_control(void *userdata, ost_transport_handle *handle, uint8_t request_type, uint8_t request,
    uint16_t value, uint16_t index, unsigned char *data, uint16_t length, unsigned int timeout_ms, int *transferred)
{
    struct mock *mock = userdata;

    (void)handle;
    (void)request_type;
    (void)request;
    (void)value;
    (void)index;
    (void)data;
    (void)timeout_ms;
    ++mock->controls;
    *transferred = length;
    return OST_SUCCESS;
}

static int mock_bulk(void *userdata, ost_transport_handle *handle, unsigned char endpoint, unsigned char *data,
    int length, unsigned int timeout_ms, int *transferred)
{
    struct mock *mock = userdata;

    (void)handle;
    (void)timeout_ms;
    if ((endpoint & 0x80u) == 0u) {
        ++mock->bulk_out;
    } else {
        ++mock->bulk_in;
        memset(data, 0x5a, (size_t)length);
        if (mock->abort_on_image_read != 0u && mock->bulk_in == mock->abort_on_image_read) {
            TEST_CHECK(ost_camera_abort(mock->camera) == OST_SUCCESS);
        }
    }
    *transferred = length;
    return OST_SUCCESS;
}

static ost_transport mock_transport(struct mock *mock)
{
    ost_transport transport;

    memset(&transport, 0, sizeof(transport));
    transport.userdata = mock;
    transport.list_devices = mock_list;
    transport.free_devices = mock_free;
    transport.open = mock_open;
    transport.close = mock_close;
    transport.control_transfer = mock_control;
    transport.bulk_transfer = mock_bulk;
    return transport;
}

static int frame_counter(void *userdata, uint32_t frame_index, const void *frame, size_t frame_size)
{
    (void)userdata;
    (void)frame_index;
    (void)frame;
    (void)frame_size;
    return OST_SUCCESS;
}

static void test_mock_transport_refuses_camera_settings_without_io(void)
{
    struct mock mock;
    ost_transport transport;
    ost_camera *camera = NULL;
    double celsius = 0.0;
    uint32_t exposure = 5u;
    ost_camera_readout readout;
    ost_sensor_geometry geometry;
    ost_stream_stats stats;

    memset(&mock, 0, sizeof(mock));
    transport = mock_transport(&mock);
    TEST_CHECK(ost_camera_open(&transport, &camera_info, &camera) == OST_SUCCESS);
    TEST_CHECK(ost_camera_get_temperature(camera, &celsius) == OST_ERR_UNSUPPORTED);
    TEST_CHECK(strstr(ost_camera_last_error(camera), "replay and mock transports") != NULL);
    TEST_CHECK(ost_camera_set_temperature(camera, -80.0) == OST_ERR_UNSUPPORTED);
    TEST_CHECK(ost_camera_set_shutter(camera, OST_SHUTTER_CLOSED) == OST_ERR_UNSUPPORTED);
    TEST_CHECK(ost_camera_set_gain(camera, 3u) == OST_ERR_UNSUPPORTED);
    TEST_CHECK(ost_camera_set_accumulations(camera, 3u) == OST_ERR_UNSUPPORTED);
    TEST_CHECK(ost_camera_set_readout_rate(camera, 100000u) == OST_ERR_UNSUPPORTED);
    TEST_CHECK(ost_camera_stream(camera, 1u, frame_counter, NULL, &stats) == OST_ERR_UNSUPPORTED);
    TEST_CHECK(ost_camera_set_exposure(camera, 100u) == OST_SUCCESS);
    TEST_CHECK(ost_camera_get_exposure(camera, &exposure) == OST_SUCCESS && exposure == 100u);
    TEST_CHECK(ost_camera_get_geometry(camera, &geometry) == OST_SUCCESS);
    TEST_CHECK(geometry.width == 1340u && geometry.height == 100u && geometry.reported == 0);
    TEST_CHECK(ost_camera_set_full_vertical_binning(camera) == OST_SUCCESS);
    TEST_CHECK(ost_camera_get_readout(camera, &readout) == OST_SUCCESS);
    TEST_CHECK(readout.width == 1340u && readout.height == 100u);
    TEST_CHECK(readout.x_binning == 1u && readout.y_binning == 100u);
    TEST_CHECK(ost_camera_query_support(camera, OST_CAMERA_OP_SET_READOUT) == OST_SUCCESS);
    TEST_CHECK(ost_camera_query_support(camera, OST_CAMERA_OP_ACQUIRE) == OST_SUCCESS);
    TEST_CHECK(ost_camera_query_support(camera, OST_CAMERA_OP_GET_TEMPERATURE) == OST_ERR_UNSUPPORTED);
    TEST_CHECK(ost_camera_query_support(camera, (ost_camera_operation)99) == OST_ERR_INVALID_ARGUMENT);
    TEST_CHECK(ost_camera_abort(camera) == OST_SUCCESS);
    TEST_CHECK(mock.opens == 1u && mock.closes == 0u);
    TEST_CHECK(mock.controls == 0u && mock.bulk_out == 0u && mock.bulk_in == 0u);
    ost_camera_close(camera);
    TEST_CHECK(mock.opens == 1u && mock.closes == 1u);
    TEST_CHECK(mock.controls == 0u && mock.bulk_out == 0u && mock.bulk_in == 0u);
}

static void test_abort_between_chunks(void)
{
    struct mock mock;
    ost_transport transport;
    unsigned char *frame = malloc(FRAME_BYTES);
    size_t acquired = 0u;

    TEST_CHECK(frame != NULL);
    memset(&mock, 0, sizeof(mock));
    transport = mock_transport(&mock);
    TEST_CHECK(ost_camera_open(&transport, &camera_info, &mock.camera) == OST_SUCCESS);
    mock.abort_on_image_read = 2u;
    TEST_CHECK(ost_camera_acquire(mock.camera, frame, FRAME_BYTES, &acquired) == OST_ERR_ABORTED);
    TEST_CHECK(acquired == 2u * CHUNK);
    TEST_CHECK(mock.bulk_in == 2u);
    mock.abort_on_image_read = 0u;
    mock.bulk_in = 0u;
    TEST_CHECK(ost_camera_acquire(mock.camera, frame, FRAME_BYTES, &acquired) == OST_SUCCESS);
    TEST_CHECK(acquired == FRAME_BYTES);
    TEST_CHECK(mock.bulk_in == (FRAME_BYTES + CHUNK - 1u) / CHUNK);
    ost_camera_close(mock.camera);
    TEST_CHECK(mock.closes == 1u);
    free(frame);
}

static void test_open_validation(void)
{
    struct mock mock;
    ost_transport transport;
    ost_camera *camera = (ost_camera *)(uintptr_t)1u;
    ost_device_info other = camera_info;
    ost_camera_open_options options;

    memset(&mock, 0, sizeof(mock));
    transport = mock_transport(&mock);
    TEST_CHECK(ost_camera_open(&transport, NULL, &camera) == OST_ERR_INVALID_ARGUMENT);
    TEST_CHECK(ost_camera_open(&transport, &camera_info, NULL) == OST_ERR_INVALID_ARGUMENT);
    other.product_id = 0x9999u;
    TEST_CHECK(ost_camera_open(&transport, &other, &camera) == OST_ERR_NOT_FOUND && camera == NULL);
    ost_camera_open_options_default(&options);
    options.target_set = 1;
    options.target_celsius = -100.0;
    TEST_CHECK(ost_camera_open_with_options(&transport, &camera_info, &options, &camera) == OST_ERR_INVALID_ARGUMENT);
    TEST_CHECK(camera == NULL);
    TEST_CHECK(strstr(ost_camera_open_error(), "USB transport") != NULL);
    TEST_CHECK(mock.opens == 0u);
    ost_camera_close(NULL);
    TEST_CHECK(strcmp(ost_camera_last_error(NULL), "no camera") == 0);
    TEST_CHECK(strcmp(ost_status_text(OST_ERR_UNSUPPORTED), "unsupported") == 0);
    TEST_CHECK(strcmp(ost_status_text(OST_ERR_ABORTED), "aborted") == 0);
    TEST_CHECK(ost_status_text(-12345) != NULL);
}

static void test_startup_data_path(void)
{
    char path[1024];

    TEST_CHECK(test_setenv("OPENST133_STARTUP_DATA", "D:/private/startup.bin", 1) == 0);
    TEST_CHECK(strcmp(ost_startup_data_path(path, sizeof(path)), "D:/private/startup.bin") == 0);
    TEST_CHECK(test_unsetenv("OPENST133_STARTUP_DATA") == 0);
    TEST_CHECK(test_setenv("OPENST133_DATA", "D:/ost-data", 1) == 0);
    TEST_CHECK(strcmp(ost_startup_data_path(path, sizeof(path)),
        "D:/ost-data/firmware/st133-startup.bin") == 0);
    TEST_CHECK(test_unsetenv("OPENST133_DATA") == 0);
#ifdef _WIN32
    TEST_CHECK(test_setenv("APPDATA", "C:/Users/test/AppData/Roaming", 1) == 0);
    TEST_CHECK(strcmp(ost_startup_data_path(path, sizeof(path)),
        "C:/Users/test/AppData/Roaming/openst133/firmware/st133-startup.bin") == 0);
#else
    TEST_CHECK(test_setenv("HOME", "/home/test", 1) == 0);
    TEST_CHECK(strcmp(ost_startup_data_path(path, sizeof(path)),
        "/home/test/.local/share/openst133/firmware/st133-startup.bin") == 0);
#endif
}

int main(int argc, char **argv)
{
    TEST_CHECK(argc == 2);
    test_replay_frame(argv[1]);
    test_replay_short_and_exhausted(argv[1]);
    test_readout_replay(argv[1]);
    test_mock_transport_refuses_camera_settings_without_io();
    test_abort_between_chunks();
    test_open_validation();
    test_startup_data_path();
    return 0;
}
