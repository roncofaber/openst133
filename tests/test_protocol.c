/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#include "openst133.h"
#include "pcap_usbmon.h"
#include "protocol.h"
#include "test_check.h"
#include "transport_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINKTYPE_USB_LINUX 189u
#define MAX_EVENTS 64u
#define MAX_DATA 32u
#define STATUS_TIMEOUT (-110)
#define STATUS_IO (-5)
#define STATUS_NO_DEVICE (-19)

static const unsigned char device_descriptor[] = {
    0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40,
    0xd7, 0x0b, 0x10, 0xa0, 0x00, 0x01, 0x01, 0x02, 0x03, 0x01,
};

/* Synthetic expected-transfer script, written as a usbmon capture for the replay transport. */
struct trace_event {
    uint8_t event;
    uint8_t transfer_type;
    uint8_t endpoint;
    uint8_t request_type;
    uint8_t request;
    uint16_t setup_value;
    uint16_t setup_index;
    uint16_t setup_length;
    int32_t status;
    uint32_t length;
    unsigned char data[MAX_DATA];
    uint32_t data_size;
};

struct script {
    struct trace_event events[MAX_EVENTS];
    size_t count;
};

static void script_init(struct script *script)
{
    struct trace_event *event;

    memset(script, 0, sizeof(*script));
    event = &script->events[script->count++];
    event->event = 'S';
    event->transfer_type = 2u;
    event->endpoint = 0x80u;
    event->request_type = 0x80u;
    event->request = 6u;
    event->setup_value = 0x0100u;
    event->setup_length = 64u;
    event->length = 64u;
    event = &script->events[script->count++];
    *event = script->events[0];
    event->event = 'C';
    event->length = sizeof(device_descriptor);
    memcpy(event->data, device_descriptor, sizeof(device_descriptor));
    event->data_size = sizeof(device_descriptor);
}

static void add_pair(
    struct script *script,
    const struct trace_event *submission,
    int32_t status,
    uint32_t completed,
    const unsigned char *completion_data,
    uint32_t completion_size)
{
    struct trace_event *event;

    TEST_CHECK(script->count + 2u <= MAX_EVENTS);
    script->events[script->count++] = *submission;
    event = &script->events[script->count++];
    *event = *submission;
    event->event = 'C';
    event->status = status;
    event->length = completed;
    event->data_size = completion_size;
    memset(event->data, 0, sizeof(event->data));
    if (completion_size != 0u) {
        TEST_CHECK(completion_size <= MAX_DATA);
        memcpy(event->data, completion_data, completion_size);
    }
}

static void add_bulk_out(struct script *script, uint8_t endpoint, const unsigned char *data, uint32_t size,
    int32_t status, uint32_t completed)
{
    struct trace_event submission;

    memset(&submission, 0, sizeof(submission));
    submission.event = 'S';
    submission.transfer_type = 3u;
    submission.endpoint = endpoint;
    submission.length = size;
    submission.data_size = size;
    TEST_CHECK(size <= MAX_DATA);
    memcpy(submission.data, data, size);
    add_pair(script, &submission, status, completed, NULL, 0u);
}

static void add_bulk_in(struct script *script, uint8_t endpoint, uint32_t requested,
    const unsigned char *data, uint32_t size, int32_t status)
{
    struct trace_event submission;

    memset(&submission, 0, sizeof(submission));
    submission.event = 'S';
    submission.transfer_type = 3u;
    submission.endpoint = endpoint;
    submission.length = requested;
    add_pair(script, &submission, status, size, data, size);
}

static void add_control(struct script *script, uint8_t request_type, uint8_t request, uint16_t value,
    const unsigned char *data, uint16_t length, int32_t status)
{
    struct trace_event submission;
    int in = (request_type & 0x80u) != 0u;

    memset(&submission, 0, sizeof(submission));
    submission.event = 'S';
    submission.transfer_type = 2u;
    submission.endpoint = in ? 0x80u : 0x00u;
    submission.request_type = request_type;
    submission.request = request;
    submission.setup_value = value;
    submission.setup_length = length;
    submission.length = length;
    if (!in) {
        submission.data_size = length;
        memcpy(submission.data, data, length);
        add_pair(script, &submission, status, length, NULL, 0u);
    } else {
        add_pair(script, &submission, status, length, data, length);
    }
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
    unsigned char word[4];
    size_t i;

    TEST_CHECK(file != NULL);
    write_le(word, 0xa1b2c3d4u, 4u);
    TEST_CHECK(fwrite(word, 1u, 4u, file) == 4u);
    {
        unsigned char rest[20];

        write_le(rest + 0, 2u, 2u);
        write_le(rest + 2, 4u, 2u);
        write_le(rest + 4, 0u, 4u);
        write_le(rest + 8, 0u, 4u);
        write_le(rest + 12, 65535u, 4u);
        write_le(rest + 16, LINKTYPE_USB_LINUX, 4u);
        TEST_CHECK(fwrite(rest, 1u, sizeof(rest), file) == sizeof(rest));
    }
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
        write_le(header + 44, event->setup_index, 2u);
        write_le(header + 46, event->setup_length, 2u);
        TEST_CHECK(fwrite(header, 1u, sizeof(header), file) == sizeof(header));
        if (event->data_size != 0u) {
            TEST_CHECK(fwrite(event->data, 1u, event->data_size, file) == event->data_size);
        }
    }
    TEST_CHECK(fclose(file) == 0);
}

static ost_replay_transport *replay;
static ost_proto *proto;

static void open_script(const char *directory, const char *name, const struct script *script)
{
    char path[1024];
    char error[256];
    ost_device_info *devices = NULL;
    size_t count = 0u;
    const ost_transport *transport;

    TEST_CHECK(snprintf(path, sizeof(path), "%s/%s", directory, name) > 0);
    write_script(path, script);
    TEST_CHECK(ost_replay_transport_create(path, &replay, error, sizeof(error)) == OST_SUCCESS);
    transport = ost_replay_transport_interface(replay);
    TEST_CHECK(ost_list_devices(transport, &devices, &count) == OST_SUCCESS);
    TEST_CHECK(count == 1u);
    TEST_CHECK(ost_proto_open(transport, &devices[0], NULL, &proto) == OST_PROTO_OK);
    ost_free_devices(devices);
}

static void close_script(void)
{
    ost_proto_close(proto);
    ost_replay_transport_destroy(replay);
    proto = NULL;
    replay = NULL;
}

/* Mock transport: records operations and returns scripted results. */
struct mock_op {
    char kind;
    unsigned char endpoint;
    int length;
    unsigned int timeout_ms;
    unsigned char data[MAX_DATA];
    uint8_t request_type;
    uint8_t request;
    uint16_t value;
    uint16_t index;
};

struct mock {
    struct mock_op ops[MAX_EVENTS];
    size_t op_count;
    int result;
    int transferred_override;
    int override_active;
    int close_count;
    int handle_marker;
};

static struct mock mock_state;

static void mock_reset(void)
{
    memset(&mock_state, 0, sizeof(mock_state));
}

static int mock_open(void *userdata, const ost_device_info *info, ost_transport_handle **handle)
{
    (void)userdata;
    (void)info;
    *handle = (ost_transport_handle *)&mock_state.handle_marker;
    return OST_SUCCESS;
}

static void mock_close(void *userdata, ost_transport_handle *handle)
{
    (void)userdata;
    (void)handle;
    ++mock_state.close_count;
}

static int mock_bulk(void *userdata, ost_transport_handle *handle, unsigned char endpoint, unsigned char *data,
    int length, unsigned int timeout_ms, int *transferred)
{
    struct mock_op *op = &mock_state.ops[mock_state.op_count++];

    (void)userdata;
    (void)handle;
    op->kind = 'b';
    op->endpoint = endpoint;
    op->length = length;
    op->timeout_ms = timeout_ms;
    if ((endpoint & 0x80u) == 0u && length > 0 && length <= (int)MAX_DATA) {
        memcpy(op->data, data, (size_t)length);
    }
    if (mock_state.result != OST_SUCCESS) {
        return mock_state.result;
    }
    *transferred = mock_state.override_active ? mock_state.transferred_override : length;
    return OST_SUCCESS;
}

static int mock_control(void *userdata, ost_transport_handle *handle, uint8_t request_type, uint8_t request,
    uint16_t value, uint16_t index, unsigned char *data, uint16_t length, unsigned int timeout_ms,
    int *transferred)
{
    struct mock_op *op = &mock_state.ops[mock_state.op_count++];

    (void)userdata;
    (void)handle;
    (void)data;
    op->kind = 'c';
    op->length = length;
    op->timeout_ms = timeout_ms;
    op->request_type = request_type;
    op->request = request;
    op->value = value;
    op->index = index;
    if (mock_state.result != OST_SUCCESS) {
        return mock_state.result;
    }
    *transferred = mock_state.override_active ? mock_state.transferred_override : length;
    return OST_SUCCESS;
}

static const ost_transport mock_transport = {
    NULL, NULL, NULL, mock_open, mock_close, mock_control, mock_bulk,
};

static const ost_device_info mock_info = {OST_USB_VENDOR_ID, OST_USB_PRODUCT_ID, 1u, 2u, 0u, {0u}};

static void mock_open_proto(void)
{
    mock_reset();
    TEST_CHECK(ost_proto_open(&mock_transport, &mock_info, NULL, &proto) == OST_PROTO_OK);
}

static void test_encoders(void)
{
    static const unsigned char single[] = {0x01, 0x34, 0x12, 0x02, 0xcd, 0xab};
    static const unsigned char request[] = {0x01, 0x42, 0x00, 0x03, 0x00, 0x00};
    static const unsigned char multi3[] = {
        0x01, 0x02, 0x01, 0x02, 0xaa, 0x00, 0x02, 0xbb, 0x00, 0x02, 0xcc, 0x00,
    };
    static const unsigned char arguments[] = {0xaa, 0xbb, 0xcc};
    static const unsigned char multi0[] = {0x01, 0x78, 0x56};
    unsigned char out[32];
    unsigned char sentinel[32];
    unsigned char response[2];

    memset(out, 0xee, sizeof(out));
    TEST_CHECK(ost_proto_encode_command(out, sizeof(out), 0x1234u, 0xabcdu) == 6u);
    TEST_CHECK(memcmp(out, single, sizeof(single)) == 0);
    TEST_CHECK(out[6] == 0xee);

    TEST_CHECK(ost_proto_encode_command_bytes(out, sizeof(out), 0x0102u, arguments, 3u) == 12u);
    TEST_CHECK(memcmp(out, multi3, sizeof(multi3)) == 0);
    TEST_CHECK(ost_proto_encode_command_bytes(out, sizeof(out), 0x5678u, NULL, 0u) == 3u);
    TEST_CHECK(memcmp(out, multi0, sizeof(multi0)) == 0);
    {
        unsigned char args[10];
        unsigned char big[3u * 10u + 3u];
        size_t i;

        for (i = 0u; i < sizeof(args); ++i) {
            args[i] = (unsigned char)(0x10u + i);
        }
        TEST_CHECK(ost_proto_encode_command_bytes(big, sizeof(big), 0x0010u, args, sizeof(args)) == sizeof(big));
        for (i = 0u; i < sizeof(args); ++i) {
            TEST_CHECK(big[3u + 3u * i] == 0x02u);
            TEST_CHECK(big[4u + 3u * i] == args[i]);
            TEST_CHECK(big[5u + 3u * i] == 0x00u);
        }
    }

    memset(out, 0xee, sizeof(out));
    memcpy(sentinel, out, sizeof(out));
    TEST_CHECK(ost_proto_encode_command_bytes(out, 11u, 0x0102u, arguments, 3u) == 0u);
    TEST_CHECK(ost_proto_encode_command(out, 5u, 1u, 2u) == 0u);
    TEST_CHECK(ost_proto_encode_command_bytes(out, sizeof(out), 1u, NULL, 1u) == 0u);
    TEST_CHECK(memcmp(out, sentinel, sizeof(out)) == 0);

    TEST_CHECK(ost_proto_encode_request(out, sizeof(out), 0x0042u) == 6u);
    TEST_CHECK(memcmp(out, request, sizeof(request)) == 0);

    response[0] = 0x34u;
    response[1] = 0x12u;
    TEST_CHECK(ost_proto_parse_response(response) == 0x1234u);
}

static void test_transport_status_mapping(void)
{
    TEST_CHECK(ost_proto_from_transport_status(OST_SUCCESS) == OST_PROTO_OK);
    TEST_CHECK(ost_proto_from_transport_status(OST_TRANSPORT_ERR_TIMEOUT) == OST_PROTO_ERR_TIMEOUT);
    TEST_CHECK(ost_proto_from_transport_status(OST_ERR_IO) == OST_PROTO_ERR_TRANSPORT);
    TEST_CHECK(ost_proto_from_transport_status(OST_ERR_NOT_FOUND) == OST_PROTO_ERR_NO_DEVICE);
    TEST_CHECK(ost_proto_from_transport_status(OST_ERR_NO_MEMORY) == OST_PROTO_ERR_NO_MEMORY);
    TEST_CHECK(ost_proto_from_transport_status(OST_ERR_UNSUPPORTED) == OST_PROTO_ERR_UNSUPPORTED);
    TEST_CHECK(ost_proto_from_transport_status(OST_ERR_INVALID_ARGUMENT) == OST_PROTO_ERR_INVALID_ARGUMENT);
    TEST_CHECK(strcmp(ost_proto_status_text(OST_PROTO_ERR_TIMEOUT), "timeout") == 0);
}

static void add_setup_script(struct script *script, uint16_t low, uint16_t high, uint16_t f2, uint16_t flag)
{
    static const unsigned char zeros[8] = {0};
    unsigned char record[6];

    ost_proto_encode_command(record, sizeof(record), 0x0014u, 0x0100u);
    add_bulk_out(script, 0x08u, record, 6u, 0, 6u);
    ost_proto_encode_command(record, sizeof(record), 0x0010u, low);
    add_bulk_out(script, 0x08u, record, 6u, 0, 6u);
    ost_proto_encode_command(record, sizeof(record), 0x0012u, high);
    add_bulk_out(script, 0x08u, record, 6u, 0, 6u);
    ost_proto_encode_command(record, sizeof(record), 0x0014u, 0x0001u);
    add_bulk_out(script, 0x08u, record, 6u, 0, 6u);
    add_control(script, 0x40u, 0xf2u, f2, zeros, 8u, 0);
    ost_proto_encode_command(record, sizeof(record), 0x0016u, flag);
    add_bulk_out(script, 0x08u, record, 6u, 0, 6u);
}

static void test_setup_order(const char *directory)
{
    struct script script;
    ost_proto_transfer_params params = {0x00012345u, 0x0200u, 0x0401u};

    script_init(&script);
    add_setup_script(&script, 0x2345u, 0x0001u, 0x0200u, 1u);
    open_script(directory, "setup-remainder.pcap", &script);
    TEST_CHECK(ost_proto_setup_transfer(proto, &params) == OST_PROTO_OK);
    TEST_CHECK(ost_proto_write_command(proto, 1u, 1u) == OST_PROTO_ERR_TRANSPORT);
    TEST_CHECK(strstr(ost_replay_transport_last_error(replay), "end of capture") != NULL);
    close_script();

    params.total_size = 0x0400u;
    script_init(&script);
    add_setup_script(&script, 0x2345u, 0x0001u, 0x0200u, 0u);
    open_script(directory, "setup-divisible.pcap", &script);
    TEST_CHECK(ost_proto_setup_transfer(proto, &params) == OST_PROTO_OK);
    close_script();

    /* The vendor request moved before the first command must be rejected by the ordered script. */
    {
        static const unsigned char zeros[8] = {0};
        unsigned char record[6];

        script_init(&script);
        add_control(&script, 0x40u, 0xf2u, 0x0200u, zeros, 8u, 0);
        ost_proto_encode_command(record, sizeof(record), 0x0014u, 0x0100u);
        add_bulk_out(&script, 0x08u, record, 6u, 0, 6u);
        open_script(directory, "setup-misordered.pcap", &script);
        TEST_CHECK(ost_proto_setup_transfer(proto, &params) == OST_PROTO_ERR_TRANSPORT);
        TEST_CHECK(strstr(ost_replay_transport_last_error(replay), "frame 3") != NULL);
        close_script();
    }
}

static void test_setup_rules(void)
{
    ost_proto_transfer_params params = {0x200u, 0u, 0x400u};

    mock_open_proto();
    TEST_CHECK(ost_proto_setup_transfer(proto, NULL) == OST_PROTO_ERR_INVALID_ARGUMENT);
    TEST_CHECK(ost_proto_setup_transfer(proto, &params) == OST_PROTO_ERR_INVALID_ARGUMENT);
    TEST_CHECK(mock_state.op_count == 0u);

    params.chunk_size = 0x200u;
    TEST_CHECK(ost_proto_setup_transfer(proto, &params) == OST_PROTO_OK);
    TEST_CHECK(mock_state.op_count == 6u);
    TEST_CHECK(mock_state.ops[0].kind == 'b' && mock_state.ops[0].endpoint == 0x08u);
    TEST_CHECK(mock_state.ops[4].kind == 'c');
    TEST_CHECK(mock_state.ops[4].request_type == 0x40u && mock_state.ops[4].request == 0xf2u);
    TEST_CHECK(mock_state.ops[4].value == 0x200u && mock_state.ops[4].index == 0u);
    TEST_CHECK(mock_state.ops[5].kind == 'b');
    ost_proto_close(proto);
}

static void test_setup_small_and_wide(const char *directory)
{
    struct script script;
    ost_proto_transfer_params params = {0x1ffu, 0u, 0x77u};

    script_init(&script);
    add_setup_script(&script, 0x01ffu, 0u, 0x01ffu, 0u);
    add_setup_script(&script, 0x0000u, 0u, 0x0000u, 0u);
    add_setup_script(&script, 0x0001u, 0u, 0x0001u, 0u);
    add_setup_script(&script, 0x0200u, 0x0001u, 0x0000u, 1u);
    open_script(directory, "setup-small.pcap", &script);
    TEST_CHECK(ost_proto_setup_transfer(proto, &params) == OST_PROTO_OK);
    params.configured_size = 0u;
    params.chunk_size = 0x500u;
    TEST_CHECK(ost_proto_setup_transfer(proto, &params) == OST_PROTO_OK);
    params.configured_size = 1u;
    TEST_CHECK(ost_proto_setup_transfer(proto, &params) == OST_PROTO_OK);
    params.configured_size = 0x10200u;
    params.chunk_size = 0x10000u;
    params.total_size = 0x10200u;
    TEST_CHECK(ost_proto_setup_transfer(proto, &params) == OST_PROTO_OK);
    close_script();
}

static const unsigned char zero8[8] = {0};
static const unsigned char reply_pre[] = {0x00, 0x00};

static void add_ordinary(struct script *script, uint8_t command, const unsigned char *response)
{
    unsigned char request[6] = {0x01, command, 0x00, 0x03, 0x00, 0x00};

    add_bulk_in(script, 0x86u, 2u, response, 2u, 0);
    add_bulk_out(script, 0x08u, request, 6u, 0, 6u);
}

static void test_input_commands(const char *directory)
{
    struct script script;
    static const unsigned char special[] = {0x01, 0x14, 0x00, 0x02, 0x01, 0x00};
    static const unsigned char r_set[] = {0x02, 0x00};
    static const unsigned char r_clear[] = {0x01, 0x80};
    uint16_t value = 0u;
    unsigned int flags = 99u;

    script_init(&script);
    add_ordinary(&script, 0x30u, reply_pre);
    add_control(&script, 0x40u, 0xf0u, 0u, zero8, 8u, 0);
    add_bulk_out(&script, 0x08u, special, 6u, 0, 6u);
    add_ordinary(&script, 0x32u, r_set);
    add_ordinary(&script, 0x42u, r_set);
    add_ordinary(&script, 0x42u, r_clear);
    open_script(directory, "input-commands.pcap", &script);
    TEST_CHECK(ost_proto_command_30(proto, &value) == OST_PROTO_OK && value == 0u);
    TEST_CHECK(ost_proto_command_32(proto, &value) == OST_PROTO_OK && value == 2u);
    TEST_CHECK(ost_proto_command_42(proto, &value, &flags) == OST_PROTO_OK);
    TEST_CHECK(value == 2u && flags == 0u);
    TEST_CHECK(ost_proto_command_42(proto, &value, &flags) == OST_PROTO_OK);
    TEST_CHECK(value == 0x8001u && flags == OST_PROTO_LOCAL_ERROR_OVERRUN);
    TEST_CHECK(ost_proto_command_42(proto, NULL, &flags) == OST_PROTO_ERR_INVALID_ARGUMENT);
    close_script();
}

struct wait_probe {
    unsigned int calls;
    unsigned int timeout_ms;
    size_t ops_at_call;
    int result;
};

static struct wait_probe wait_state;

static int probe_wait(void *context, unsigned int timeout_ms)
{
    (void)context;
    ++wait_state.calls;
    wait_state.timeout_ms = timeout_ms;
    wait_state.ops_at_call = mock_state.op_count;
    return wait_state.result;
}

static void test_input_command_timeouts(void)
{
    uint16_t value = 0u;
    unsigned int flags = 0u;

    mock_open_proto();
    TEST_CHECK(ost_proto_command_30(proto, &value) == OST_PROTO_OK);
    TEST_CHECK(mock_state.op_count == 2u);
    TEST_CHECK(mock_state.ops[0].endpoint == 0x86u && mock_state.ops[0].length == 2);
    TEST_CHECK(mock_state.ops[0].timeout_ms == 10000u && mock_state.ops[1].timeout_ms == 10000u);
    TEST_CHECK(mock_state.ops[1].length == 6 && mock_state.ops[1].data[1] == 0x30u);
    TEST_CHECK(ost_proto_command_42(proto, &value, &flags) == OST_PROTO_OK);
    TEST_CHECK(mock_state.ops[3].data[1] == 0x42u && mock_state.ops[3].timeout_ms == 10000u);

    memset(&wait_state, 0, sizeof(wait_state));
    mock_state.op_count = 0u;
    TEST_CHECK(ost_proto_command_32(proto, &value) == OST_PROTO_OK);
    TEST_CHECK(wait_state.calls == 0u && mock_state.op_count == 4u);
    TEST_CHECK(mock_state.ops[0].kind == 'c' && mock_state.ops[0].request == 0xf0u);
    TEST_CHECK(mock_state.ops[0].timeout_ms == OST_PROTO_DEFAULT_CONTROL_TIMEOUT_MS);
    TEST_CHECK(mock_state.ops[1].endpoint == 0x08u && mock_state.ops[1].timeout_ms == 10000u);
    TEST_CHECK(mock_state.ops[1].data[1] == 0x14u && mock_state.ops[1].data[4] == 0x01u);
    TEST_CHECK(mock_state.ops[3].data[1] == 0x32u);

    ost_proto_set_worker_wait(proto, probe_wait, NULL);
    mock_state.op_count = 0u;
    TEST_CHECK(ost_proto_command_32(proto, &value) == OST_PROTO_OK);
    TEST_CHECK(wait_state.calls == 1u && wait_state.timeout_ms == 5000u && wait_state.ops_at_call == 2u);
    TEST_CHECK(mock_state.op_count == 4u);

    wait_state.result = OST_PROTO_ERR_TIMEOUT;
    mock_state.op_count = 0u;
    TEST_CHECK(ost_proto_command_32(proto, &value) == OST_PROTO_ERR_TIMEOUT);
    TEST_CHECK(mock_state.op_count == 2u);

    wait_state.result = OST_PROTO_OK;
    mock_state.op_count = 0u;
    mock_state.result = OST_TRANSPORT_ERR_TIMEOUT;
    TEST_CHECK(ost_proto_command_32(proto, &value) == OST_PROTO_ERR_TIMEOUT);
    TEST_CHECK(mock_state.op_count == 1u && wait_state.calls == 2u);
    mock_state.result = OST_SUCCESS;
    ost_proto_close(proto);
}

static void test_input_command_errors(const char *directory)
{
    struct script script;
    static const unsigned char special[] = {0x01, 0x14, 0x00, 0x02, 0x01, 0x00};
    uint16_t value = 0xffffu;
    unsigned int flags = 77u;

    script_init(&script);
    add_control(&script, 0x40u, 0xf0u, 0u, zero8, 8u, STATUS_TIMEOUT);
    add_control(&script, 0x40u, 0xf0u, 0u, zero8, 8u, 0);
    add_bulk_out(&script, 0x08u, special, 6u, 0, 3u);
    add_control(&script, 0x40u, 0xf0u, 0u, zero8, 8u, 0);
    add_bulk_out(&script, 0x08u, special, 6u, 0, 6u);
    add_bulk_in(&script, 0x86u, 2u, NULL, 0u, STATUS_TIMEOUT);
    add_bulk_in(&script, 0x86u, 2u, NULL, 0u, STATUS_TIMEOUT);
    open_script(directory, "input-errors.pcap", &script);
    TEST_CHECK(ost_proto_command_32(proto, &value) == OST_PROTO_ERR_TIMEOUT);
    TEST_CHECK(ost_proto_command_32(proto, &value) == OST_PROTO_ERR_SHORT_TRANSFER);
    TEST_CHECK(ost_proto_command_32(proto, &value) == OST_PROTO_ERR_TIMEOUT);
    TEST_CHECK(ost_proto_command_42(proto, &value, &flags) == OST_PROTO_ERR_TIMEOUT);
    TEST_CHECK(value == 0xffffu && flags == 77u);
    close_script();
}

static void test_commands_replay(const char *directory)
{
    struct script script;
    static const unsigned char arguments[] = {0x11, 0x22, 0x33};
    static const unsigned char multi[] = {0x01, 0x30, 0x00, 0x02, 0x11, 0x00, 0x02, 0x22, 0x00, 0x02, 0x33, 0x00};
    static const unsigned char single[] = {0x01, 0x34, 0x12, 0x02, 0xcd, 0xab};
    static const unsigned char response[] = {0x34, 0x12};
    static const unsigned char request[] = {0x01, 0x42, 0x00, 0x03, 0x00, 0x00};
    uint16_t value = 0u;

    script_init(&script);
    add_bulk_out(&script, 0x08u, single, 6u, 0, 6u);
    add_bulk_out(&script, 0x08u, multi, 12u, 0, 12u);
    add_bulk_in(&script, 0x86u, 2u, response, 2u, 0);
    add_bulk_out(&script, 0x08u, request, 6u, 0, 6u);
    open_script(directory, "commands.pcap", &script);
    TEST_CHECK(ost_proto_write_command(proto, 0x1234u, 0xabcdu) == OST_PROTO_OK);
    TEST_CHECK(ost_proto_write_command_bytes(proto, 0x0030u, arguments, 3u) == OST_PROTO_OK);
    TEST_CHECK(ost_proto_read_value(proto, 0x0042u, &value) == OST_PROTO_OK);
    TEST_CHECK(value == 0x1234u);
    close_script();
}

static void test_error_mapping_replay(const char *directory)
{
    struct script script;
    static const unsigned char single[] = {0x01, 0x34, 0x12, 0x02, 0xcd, 0xab};
    static const unsigned char partial[] = {0x34};
    static const unsigned char response[] = {0x34, 0x12};
    static const unsigned char request[] = {0x01, 0x42, 0x00, 0x03, 0x00, 0x00};
    uint16_t value = 0xffffu;

    script_init(&script);
    add_bulk_out(&script, 0x08u, single, 6u, STATUS_TIMEOUT, 0u);
    add_bulk_out(&script, 0x08u, single, 6u, 0, 3u);
    add_bulk_out(&script, 0x08u, single, 6u, STATUS_IO, 0u);
    add_bulk_out(&script, 0x08u, single, 6u, STATUS_NO_DEVICE, 0u);
    add_bulk_in(&script, 0x86u, 2u, NULL, 0u, STATUS_TIMEOUT);
    add_bulk_in(&script, 0x86u, 2u, partial, 1u, 0);
    add_bulk_in(&script, 0x86u, 2u, response, 2u, 0);
    add_bulk_out(&script, 0x08u, request, 6u, 0, 3u);
    open_script(directory, "errors.pcap", &script);
    TEST_CHECK(ost_proto_write_command(proto, 0x1234u, 0xabcdu) == OST_PROTO_ERR_TIMEOUT);
    TEST_CHECK(ost_proto_write_command(proto, 0x1234u, 0xabcdu) == OST_PROTO_ERR_SHORT_TRANSFER);
    TEST_CHECK(ost_proto_write_command(proto, 0x1234u, 0xabcdu) == OST_PROTO_ERR_TRANSPORT);
    TEST_CHECK(ost_proto_write_command(proto, 0x1234u, 0xabcdu) == OST_PROTO_ERR_NO_DEVICE);
    TEST_CHECK(ost_proto_read_value(proto, 0x0042u, &value) == OST_PROTO_ERR_TIMEOUT);
    TEST_CHECK(ost_proto_read_value(proto, 0x0042u, &value) == OST_PROTO_ERR_SHORT_TRANSFER);
    TEST_CHECK(value == 0xffffu);
    TEST_CHECK(ost_proto_read_value(proto, 0x0042u, &value) == OST_PROTO_ERR_SHORT_TRANSFER);
    TEST_CHECK(value == 0xffffu);
    close_script();
}

static void test_vendor_replay(const char *directory)
{
    struct script script;
    static const unsigned char zeros[8] = {0};
    static const unsigned char reply[] = {0xde, 0xad, 0xbe, 0xef};
    unsigned char out[4] = {0};
    uint16_t f1 = 0u;

    script_init(&script);
    add_control(&script, 0x40u, 0xf0u, 0u, zeros, 8u, 0);
    add_control(&script, 0x40u, 0xf2u, 0x1234u, zeros, 8u, 0);
    add_control(&script, 0xc0u, 0xf3u, 0u, reply, 4u, 0);
    add_control(&script, 0xc0u, 0xf1u, 0u, reply, 2u, 0);
    add_control(&script, 0x40u, 0xf0u, 0u, zeros, 8u, STATUS_TIMEOUT);
    open_script(directory, "vendor.pcap", &script);
    TEST_CHECK(ost_proto_vendor_f0(proto) == OST_PROTO_OK);
    TEST_CHECK(ost_proto_vendor_f2(proto, 0x1234u) == OST_PROTO_OK);
    TEST_CHECK(ost_proto_vendor_f3(proto, out) == OST_PROTO_OK);
    TEST_CHECK(memcmp(out, reply, sizeof(reply)) == 0);
    TEST_CHECK(ost_proto_vendor_f1(proto, &f1) == OST_PROTO_OK && f1 == 0xadde);
    TEST_CHECK(ost_proto_vendor_f1(proto, NULL) == OST_PROTO_ERR_INVALID_ARGUMENT);
    TEST_CHECK(ost_proto_vendor_f0(proto) == OST_PROTO_ERR_TIMEOUT);
    close_script();
}

static void test_image_replay(const char *directory)
{
    struct script script;
    static const unsigned char first[] = {1};
    static const unsigned char second[] = {2, 3};
    static const unsigned char third[] = {4, 5};
    unsigned char buffer[8] = {0};
    size_t received = 99u;

    script_init(&script);
    add_bulk_in(&script, 0x82u, 2u, first, 1u, 0);
    add_bulk_in(&script, 0x82u, 2u, second, 2u, 0);
    add_bulk_in(&script, 0x82u, 2u, third, 2u, 0);
    add_bulk_in(&script, 0x82u, 2u, NULL, 0u, 0);
    open_script(directory, "image.pcap", &script);
    TEST_CHECK(ost_proto_read_image(proto, buffer, 5u, 2u, &received) == OST_PROTO_OK);
    TEST_CHECK(received == 5u);
    TEST_CHECK(memcmp(buffer, "\x01\x02\x03\x04\x05", 5u) == 0);
    TEST_CHECK(ost_proto_read_image(proto, buffer, 2u, 2u, &received) == OST_PROTO_ERR_SHORT_TRANSFER);
    TEST_CHECK(received == 0u);
    close_script();
}

static void test_image_chunks(const char *directory)
{
    struct script script;
    static const unsigned char a[] = {1, 2, 3};
    static const unsigned char b[] = {4, 5};
    static const unsigned char c[] = {6};
    unsigned char buffer[8] = {0};
    size_t received = 99u;
    size_t taken = 99u;

    script_init(&script);
    add_bulk_in(&script, 0x82u, 4u, a, 3u, 0);
    add_bulk_in(&script, 0x82u, 3u, b, 2u, 0);
    add_bulk_in(&script, 0x82u, 1u, c, 1u, 0);
    add_bulk_in(&script, 0x82u, 4u, a, 3u, 0);
    add_bulk_in(&script, 0x82u, 4u, NULL, 0u, STATUS_TIMEOUT);
    open_script(directory, "image-chunks.pcap", &script);
    TEST_CHECK(ost_proto_multiple_input_take(proto, &taken) == OST_PROTO_OK && taken == 0u);
    TEST_CHECK(ost_proto_read_image(proto, buffer, 6u, 4u, &received) == OST_PROTO_OK);
    TEST_CHECK(received == 6u && memcmp(buffer, "\x01\x02\x03\x04\x05\x06", 6u) == 0);
    TEST_CHECK(ost_proto_multiple_input_take(proto, &taken) == OST_PROTO_OK && taken == 6u);
    TEST_CHECK(ost_proto_multiple_input_take(proto, &taken) == OST_PROTO_OK && taken == 0u);
    TEST_CHECK(ost_proto_read_image(proto, buffer, 8u, 4u, &received) == OST_PROTO_ERR_TIMEOUT);
    TEST_CHECK(received == 3u);
    TEST_CHECK(ost_proto_multiple_input_take(proto, &taken) == OST_PROTO_OK && taken == 0u);
    TEST_CHECK(ost_proto_read_image(proto, buffer, 0u, 4u, &received) == OST_PROTO_OK && received == 0u);
    close_script();
}

static void test_timeouts_and_stop(void)
{
    ost_proto_config config;
    unsigned char buffer[8000];
    size_t received = 0u;
    uint16_t value = 0u;

    mock_open_proto();
    TEST_CHECK(ost_proto_write_command(proto, 1u, 2u) == OST_PROTO_OK);
    TEST_CHECK(mock_state.ops[0].timeout_ms == 10000u);
    TEST_CHECK(ost_proto_write_command_bytes(proto, 1u, (const unsigned char *)"ab", 2u) == OST_PROTO_OK);
    TEST_CHECK(mock_state.ops[1].length == 9 && mock_state.ops[1].timeout_ms == 10000u);
    TEST_CHECK(ost_proto_vendor_f0(proto) == OST_PROTO_OK);
    TEST_CHECK(mock_state.ops[2].timeout_ms == OST_PROTO_DEFAULT_CONTROL_TIMEOUT_MS);
    TEST_CHECK(ost_proto_read_value(proto, 0x42u, &value) == OST_PROTO_OK);
    TEST_CHECK(mock_state.ops[3].endpoint == 0x86u && mock_state.ops[3].timeout_ms == 10000u);
    TEST_CHECK(mock_state.ops[4].endpoint == 0x08u);
    TEST_CHECK(ost_proto_read_image(proto, buffer, 8000u, 4000u, &received) == OST_PROTO_OK);
    TEST_CHECK(received == 8000u);
    TEST_CHECK(mock_state.ops[5].endpoint == 0x82u && mock_state.ops[5].length == 4000);
    TEST_CHECK(mock_state.ops[5].timeout_ms == 4000u / 2000u + 1000u);
    TEST_CHECK(mock_state.ops[6].timeout_ms == 4000u / 2000u + 1000u);

    mock_state.result = OST_TRANSPORT_ERR_TIMEOUT;
    TEST_CHECK(ost_proto_read_image(proto, buffer, 10u, 10u, &received) == OST_PROTO_ERR_TIMEOUT);
    mock_state.result = OST_SUCCESS;
    mock_state.override_active = 1;
    mock_state.transferred_override = 100;
    TEST_CHECK(ost_proto_read_image(proto, buffer, 10u, 10u, &received) == OST_PROTO_ERR_TRANSPORT);

    ost_proto_request_stop(proto);
    mock_state.override_active = 0;
    {
        size_t before = mock_state.op_count;

        TEST_CHECK(ost_proto_read_image(proto, buffer, 10u, 10u, &received) == OST_PROTO_ERR_STOPPED);
        TEST_CHECK(received == 0u && mock_state.op_count == before);
    }
    ost_proto_close(proto);

    ost_proto_config_default(&config);
    config.command_write_timeout_ms = 0u;
    mock_reset();
    TEST_CHECK(ost_proto_open(&mock_transport, &mock_info, &config, &proto) == OST_PROTO_ERR_INVALID_ARGUMENT);
    TEST_CHECK(mock_state.close_count == 0 && mock_state.op_count == 0u);
}

struct close_trace {
    unsigned int steps[8];
    size_t count;
    size_t transport_closes_at_device_step;
};

static void close_observer(void *context, unsigned int step)
{
    struct close_trace *trace = context;

    if (trace->count < 8u) {
        trace->steps[trace->count++] = step;
    }
    if (step == OST_PROTO_CLOSE_STEP_DEVICE) {
        trace->transport_closes_at_device_step = (size_t)mock_state.close_count;
    }
}

static void test_close(void)
{
    struct close_trace trace;

    memset(&trace, 0, sizeof(trace));
    mock_open_proto();
    TEST_CHECK(ost_proto_open_pipes(proto) == OST_PROTO_OK);
    TEST_CHECK(ost_proto_open_pipes(proto) == OST_PROTO_OK);
    TEST_CHECK(mock_state.op_count == 0u);
    ost_proto_set_close_hook(proto, close_observer, &trace);
    ost_proto_close(proto);
    TEST_CHECK(trace.count == 4u);
    TEST_CHECK(trace.steps[0] == OST_PROTO_CLOSE_STEP_DEVICE);
    TEST_CHECK(trace.steps[1] == OST_PROTO_PIPE_RESPONSE_IN);
    TEST_CHECK(trace.steps[2] == OST_PROTO_PIPE_COMMAND_OUT);
    TEST_CHECK(trace.steps[3] == OST_PROTO_PIPE_IMAGE_IN);
    TEST_CHECK(trace.transport_closes_at_device_step == 0u);
    TEST_CHECK(mock_state.close_count == 1);
    TEST_CHECK(mock_state.op_count == 0u);
    ost_proto_close(NULL);

    mock_open_proto();
    TEST_CHECK(ost_proto_write_command(proto, 1u, 1u) == OST_PROTO_OK);
    TEST_CHECK(mock_state.op_count == 1u);
    ost_proto_close(proto);
    TEST_CHECK(mock_state.op_count == 1u);
    TEST_CHECK(mock_state.close_count == 1);
}

int main(int argc, char **argv)
{
    TEST_CHECK(argc == 2);
    test_encoders();
    test_transport_status_mapping();
    test_setup_order(argv[1]);
    test_setup_rules();
    test_setup_small_and_wide(argv[1]);
    test_input_commands(argv[1]);
    test_input_command_timeouts();
    test_input_command_errors(argv[1]);
    test_image_chunks(argv[1]);
    test_commands_replay(argv[1]);
    test_error_mapping_replay(argv[1]);
    test_vendor_replay(argv[1]);
    test_image_replay(argv[1]);
    test_timeouts_and_stop();
    test_close();
    return 0;
}
