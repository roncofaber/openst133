/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#include "protocol.h"

#include "transport_internal.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct ost_proto {
    ost_device *device;
    ost_proto_config config;
    int pipe_open[OST_PROTO_PIPE_COUNT];
    atomic_int closed;
    atomic_int stop;
    atomic_size_t input_completed;
    atomic_int input_signaled;
    ost_proto_worker_wait worker_wait;
    void *worker_wait_context;
    ost_proto_close_hook close_hook;
    void *close_hook_context;
};

void ost_proto_config_default(ost_proto_config *config)
{
    if (config == NULL) {
        return;
    }
    config->pipe_endpoint[OST_PROTO_PIPE_IMAGE_IN] = OST_PROTO_CANDIDATE_ENDPOINT_PIPE0;
    config->pipe_endpoint[OST_PROTO_PIPE_RESPONSE_IN] = OST_PROTO_CANDIDATE_ENDPOINT_PIPE1;
    config->pipe_endpoint[OST_PROTO_PIPE_COMMAND_OUT] = OST_PROTO_CANDIDATE_ENDPOINT_PIPE2;
    config->command_write_timeout_ms = OST_PROTO_COMMAND_WRITE_TIMEOUT_MS;
    config->multi_write_timeout_ms = OST_PROTO_COMMAND_WRITE_TIMEOUT_MS;
    config->response_read_timeout_ms = OST_PROTO_DEFAULT_RESPONSE_READ_TIMEOUT_MS;
    config->control_timeout_ms = OST_PROTO_DEFAULT_CONTROL_TIMEOUT_MS;
}

int ost_proto_from_transport_status(int transport_status)
{
    switch (transport_status) {
    case OST_SUCCESS:
        return OST_PROTO_OK;
    case OST_TRANSPORT_ERR_TIMEOUT:
    case OST_ERR_TIMEOUT:
        return OST_PROTO_ERR_TIMEOUT;
    case OST_ERR_INVALID_ARGUMENT:
        return OST_PROTO_ERR_INVALID_ARGUMENT;
    case OST_ERR_NO_MEMORY:
        return OST_PROTO_ERR_NO_MEMORY;
    case OST_ERR_NOT_FOUND:
        return OST_PROTO_ERR_NO_DEVICE;
    case OST_ERR_UNSUPPORTED:
        return OST_PROTO_ERR_UNSUPPORTED;
    default:
        return OST_PROTO_ERR_TRANSPORT;
    }
}

const char *ost_proto_status_text(int status)
{
    switch (status) {
    case OST_PROTO_OK:
        return "success";
    case OST_PROTO_ERR_INVALID_ARGUMENT:
        return "invalid argument";
    case OST_PROTO_ERR_NO_MEMORY:
        return "out of memory";
    case OST_PROTO_ERR_TRANSPORT:
        return "transport error";
    case OST_PROTO_ERR_NO_DEVICE:
        return "device not found";
    case OST_PROTO_ERR_UNSUPPORTED:
        return "unsupported";
    case OST_PROTO_ERR_TIMEOUT:
        return "timeout";
    case OST_PROTO_ERR_SHORT_TRANSFER:
        return "short transfer";
    case OST_PROTO_ERR_CLOSED:
        return "closed";
    case OST_PROTO_ERR_STOPPED:
        return "stopped";
    default:
        return "unknown status";
    }
}

size_t ost_proto_encode_command(unsigned char *out, size_t capacity, uint16_t command, uint16_t argument)
{
    if (out == NULL || capacity < OST_PROTO_SINGLE_COMMAND_SIZE) {
        return 0u;
    }
    out[0] = OST_PROTO_RECORD_LEAD;
    out[1] = (unsigned char)(command & 0xffu);
    out[2] = (unsigned char)(command >> 8u);
    out[3] = OST_PROTO_RECORD_ARG_TAG;
    out[4] = (unsigned char)(argument & 0xffu);
    out[5] = (unsigned char)(argument >> 8u);
    return OST_PROTO_SINGLE_COMMAND_SIZE;
}

size_t ost_proto_encode_command_bytes(
    unsigned char *out,
    size_t capacity,
    uint16_t command,
    const unsigned char *arguments,
    size_t argument_count)
{
    size_t size;
    size_t i;

    if (out == NULL || (argument_count != 0u && arguments == NULL) ||
        argument_count > (SIZE_MAX - 3u) / 3u) {
        return 0u;
    }
    size = 3u * argument_count + 3u;
    if (capacity < size) {
        return 0u;
    }
    out[0] = OST_PROTO_RECORD_LEAD;
    out[1] = (unsigned char)(command & 0xffu);
    out[2] = (unsigned char)(command >> 8u);
    for (i = 0u; i < argument_count; ++i) {
        out[3u + 3u * i] = OST_PROTO_RECORD_ARG_TAG;
        out[4u + 3u * i] = arguments[i];
        out[5u + 3u * i] = 0u;
    }
    return size;
}

size_t ost_proto_encode_request(unsigned char *out, size_t capacity, uint16_t command)
{
    if (out == NULL || capacity < OST_PROTO_SINGLE_COMMAND_SIZE) {
        return 0u;
    }
    out[0] = OST_PROTO_RECORD_LEAD;
    out[1] = (unsigned char)(command & 0xffu);
    out[2] = (unsigned char)(command >> 8u);
    out[3] = OST_PROTO_RECORD_REQUEST_TAG;
    out[4] = 0u;
    out[5] = 0u;
    return OST_PROTO_SINGLE_COMMAND_SIZE;
}

uint16_t ost_proto_parse_response(const unsigned char response[OST_PROTO_RESPONSE_SIZE])
{
    return (uint16_t)(((uint16_t)response[1] << 8u) | response[0]);
}

static int config_is_valid(const ost_proto_config *config)
{
    return config->command_write_timeout_ms != 0u && config->multi_write_timeout_ms != 0u &&
        config->response_read_timeout_ms != 0u && config->control_timeout_ms != 0u;
}

int ost_proto_open(
    const ost_transport *transport,
    const ost_device_info *info,
    const ost_proto_config *config,
    ost_proto **proto)
{
    ost_proto *opened;
    int status;

    if (info == NULL || proto == NULL) {
        return OST_PROTO_ERR_INVALID_ARGUMENT;
    }
    *proto = NULL;
    opened = calloc(1u, sizeof(*opened));
    if (opened == NULL) {
        return OST_PROTO_ERR_NO_MEMORY;
    }
    if (config != NULL) {
        opened->config = *config;
    } else {
        ost_proto_config_default(&opened->config);
    }
    if (!config_is_valid(&opened->config)) {
        free(opened);
        return OST_PROTO_ERR_INVALID_ARGUMENT;
    }
    status = ost_open(transport, info, &opened->device);
    if (status != OST_SUCCESS) {
        free(opened);
        return ost_proto_from_transport_status(status);
    }
    atomic_init(&opened->closed, 0);
    atomic_init(&opened->stop, 0);
    atomic_init(&opened->input_completed, 0u);
    atomic_init(&opened->input_signaled, 0);
    status = ost_proto_open_pipes(opened);
    if (status != OST_PROTO_OK) {
        ost_close(opened->device);
        free(opened);
        return status;
    }
    *proto = opened;
    return OST_PROTO_OK;
}

int ost_proto_open_pipes(ost_proto *proto)
{
    static const unsigned int order[OST_PROTO_PIPE_COUNT] = {
        OST_PROTO_PIPE_RESPONSE_IN,
        OST_PROTO_PIPE_COMMAND_OUT,
        OST_PROTO_PIPE_IMAGE_IN,
    };
    size_t i;

    if (proto == NULL) {
        return OST_PROTO_ERR_INVALID_ARGUMENT;
    }
    if (atomic_load(&proto->closed) != 0) {
        return OST_PROTO_ERR_CLOSED;
    }
    for (i = 0u; i < OST_PROTO_PIPE_COUNT; ++i) {
        if (!proto->pipe_open[order[i]]) {
            proto->pipe_open[order[i]] = 1;
        }
    }
    return OST_PROTO_OK;
}

static int check_usable(const ost_proto *proto, unsigned int pipe)
{
    if (proto == NULL) {
        return OST_PROTO_ERR_INVALID_ARGUMENT;
    }
    if (atomic_load(&proto->closed) != 0) {
        return OST_PROTO_ERR_CLOSED;
    }
    if (pipe < OST_PROTO_PIPE_COUNT && !proto->pipe_open[pipe]) {
        return OST_PROTO_ERR_CLOSED;
    }
    return OST_PROTO_OK;
}

static int pipe_transfer(
    ost_proto *proto,
    unsigned int pipe,
    unsigned char *data,
    size_t length,
    unsigned int timeout_ms,
    size_t *transferred)
{
    int moved = 0;
    int status = check_usable(proto, pipe);

    *transferred = 0u;
    if (status != OST_PROTO_OK) {
        return status;
    }
    if (pipe >= OST_PROTO_PIPE_COUNT || length > (size_t)INT_MAX || timeout_ms == 0u) {
        return OST_PROTO_ERR_INVALID_ARGUMENT;
    }
    status = ost_bulk_transfer(
        proto->device, proto->config.pipe_endpoint[pipe], data, (int)length, timeout_ms, &moved);
    if (status != OST_SUCCESS) {
        return ost_proto_from_transport_status(status);
    }
    if (moved < 0 || (size_t)moved > length) {
        return OST_PROTO_ERR_TRANSPORT;
    }
    *transferred = (size_t)moved;
    return OST_PROTO_OK;
}

static int pipe_write_all(
    ost_proto *proto,
    unsigned int pipe,
    const unsigned char *data,
    size_t length,
    unsigned int timeout_ms)
{
    size_t moved;
    int status = pipe_transfer(proto, pipe, (unsigned char *)(uintptr_t)data, length, timeout_ms, &moved);

    if (status != OST_PROTO_OK) {
        return status;
    }
    return moved == length ? OST_PROTO_OK : OST_PROTO_ERR_SHORT_TRANSFER;
}

int ost_proto_write_command(ost_proto *proto, uint16_t command, uint16_t argument)
{
    unsigned char record[OST_PROTO_SINGLE_COMMAND_SIZE];
    size_t size = ost_proto_encode_command(record, sizeof(record), command, argument);

    if (proto == NULL) {
        return OST_PROTO_ERR_INVALID_ARGUMENT;
    }
    return pipe_write_all(
        proto, OST_PROTO_PIPE_COMMAND_OUT, record, size, proto->config.command_write_timeout_ms);
}

int ost_proto_write_command_bytes(
    ost_proto *proto,
    uint16_t command,
    const unsigned char *arguments,
    size_t argument_count)
{
    unsigned char *record;
    size_t size;
    int status;

    if (proto == NULL || (argument_count != 0u && arguments == NULL) ||
        argument_count > ((size_t)INT_MAX - 3u) / 3u) {
        return OST_PROTO_ERR_INVALID_ARGUMENT;
    }
    size = 3u * argument_count + 3u;
    record = malloc(size);
    if (record == NULL) {
        return OST_PROTO_ERR_NO_MEMORY;
    }
    size = ost_proto_encode_command_bytes(record, size, command, arguments, argument_count);
    status = pipe_write_all(
        proto, OST_PROTO_PIPE_COMMAND_OUT, record, size, proto->config.multi_write_timeout_ms);
    free(record);
    return status;
}

int ost_proto_read_value(ost_proto *proto, uint16_t command, uint16_t *value)
{
    unsigned char response[OST_PROTO_RESPONSE_SIZE] = {0};
    unsigned char request[OST_PROTO_SINGLE_COMMAND_SIZE];
    size_t moved;
    int status;

    if (proto == NULL || value == NULL) {
        return OST_PROTO_ERR_INVALID_ARGUMENT;
    }
    status = pipe_transfer(
        proto, OST_PROTO_PIPE_RESPONSE_IN, response, sizeof(response),
        proto->config.response_read_timeout_ms, &moved);
    if (status != OST_PROTO_OK) {
        return status;
    }
    if (moved != sizeof(response)) {
        return OST_PROTO_ERR_SHORT_TRANSFER;
    }
    status = pipe_write_all(
        proto, OST_PROTO_PIPE_COMMAND_OUT, request,
        ost_proto_encode_request(request, sizeof(request), command),
        proto->config.command_write_timeout_ms);
    if (status != OST_PROTO_OK) {
        return status;
    }
    *value = ost_proto_parse_response(response);
    return OST_PROTO_OK;
}

static unsigned int image_timeout_ms(size_t requested)
{
    uint64_t timeout = (uint64_t)requested / OST_PROTO_IMAGE_TIMEOUT_DIVISOR + OST_PROTO_IMAGE_TIMEOUT_BASE_MS;

    return timeout > UINT_MAX ? UINT_MAX : (unsigned int)timeout;
}

int ost_proto_read_image(ost_proto *proto, unsigned char *buffer, size_t total, size_t chunk_size, size_t *received)
{
    size_t done = 0u;
    int status = OST_PROTO_OK;

    if (received != NULL) {
        *received = 0u;
    }
    if (proto != NULL) {
        atomic_store(&proto->input_completed, 0u);
        atomic_store(&proto->input_signaled, 0);
    }
    if (proto == NULL || received == NULL || chunk_size == 0u || (total != 0u && buffer == NULL)) {
        return OST_PROTO_ERR_INVALID_ARGUMENT;
    }
    while (done < total) {
        size_t remaining = total - done;
        size_t requested = remaining < chunk_size ? remaining : chunk_size;
        size_t moved;

        if (atomic_load(&proto->stop) != 0) {
            status = OST_PROTO_ERR_STOPPED;
            break;
        }
        if (requested > (size_t)INT_MAX) {
            requested = (size_t)INT_MAX;
        }
        status = pipe_transfer(
            proto, OST_PROTO_PIPE_IMAGE_IN, buffer + done, requested, image_timeout_ms(requested), &moved);
        if (status != OST_PROTO_OK) {
            break;
        }
        if (moved == 0u) {
            status = OST_PROTO_ERR_SHORT_TRANSFER;
            break;
        }
        done += moved;
        atomic_fetch_add(&proto->input_completed, moved);
    }
    *received = done;
    if (status == OST_PROTO_OK) {
        atomic_store(&proto->input_signaled, 1);
    }
    return status;
}

int ost_proto_multiple_input_take(ost_proto *proto, size_t *completed)
{
    if (proto == NULL || completed == NULL) {
        return OST_PROTO_ERR_INVALID_ARGUMENT;
    }
    *completed = 0u;
    if (atomic_exchange(&proto->input_signaled, 0) != 0) {
        *completed = atomic_exchange(&proto->input_completed, 0u);
    }
    return OST_PROTO_OK;
}

static int vendor_transfer(
    ost_proto *proto,
    uint8_t request_type,
    uint8_t request,
    uint16_t first_argument,
    unsigned char *data,
    uint16_t length)
{
    int moved = 0;
    int status = check_usable(proto, OST_PROTO_PIPE_COUNT);

    if (status != OST_PROTO_OK) {
        return status;
    }
    status = ost_control_transfer(
        proto->device, request_type, request, first_argument, 0u, data, length,
        proto->config.control_timeout_ms, &moved);
    if (status != OST_SUCCESS) {
        return ost_proto_from_transport_status(status);
    }
    if (moved < 0) {
        return OST_PROTO_ERR_TRANSPORT;
    }
    return moved == (int)length ? OST_PROTO_OK : OST_PROTO_ERR_SHORT_TRANSFER;
}

static int vendor_out(ost_proto *proto, uint8_t request, uint16_t first_argument)
{
    unsigned char data[OST_PROTO_VENDOR_OUT_SIZE] = {0};

    return vendor_transfer(
        proto, OST_PROTO_VENDOR_REQTYPE_OUT, request, first_argument, data, sizeof(data));
}

int ost_proto_vendor_f0(ost_proto *proto)
{
    return vendor_out(proto, OST_PROTO_VENDOR_REQ_F0, 0u);
}

int ost_proto_vendor_f2(ost_proto *proto, uint16_t argument)
{
    return vendor_out(proto, OST_PROTO_VENDOR_REQ_F2, argument);
}

int ost_proto_vendor_f1(ost_proto *proto, uint16_t *value)
{
    unsigned char data[OST_PROTO_VENDOR_F1_IN_SIZE] = {0};
    int status;

    if (value == NULL) {
        return OST_PROTO_ERR_INVALID_ARGUMENT;
    }
    status = vendor_transfer(
        proto, OST_PROTO_VENDOR_REQTYPE_IN, OST_PROTO_VENDOR_REQ_F1, 0u, data, sizeof(data));
    if (status == OST_PROTO_OK) {
        *value = (uint16_t)(data[0] + 256u * data[1]);
    }
    return status;
}

int ost_proto_vendor_f3(ost_proto *proto, unsigned char response[OST_PROTO_VENDOR_IN_SIZE])
{
    unsigned char data[OST_PROTO_VENDOR_IN_SIZE] = {0};
    int status;

    if (response == NULL) {
        return OST_PROTO_ERR_INVALID_ARGUMENT;
    }
    status = vendor_transfer(
        proto, OST_PROTO_VENDOR_REQTYPE_IN, OST_PROTO_VENDOR_REQ_F3, 0u, data, sizeof(data));
    if (status == OST_PROTO_OK) {
        memcpy(response, data, sizeof(data));
    }
    return status;
}

int ost_proto_setup_transfer(ost_proto *proto, const ost_proto_transfer_params *params)
{
    uint16_t low;
    uint16_t high;
    uint16_t remainder_flag;
    uint16_t f2_argument;
    int status;

    if (proto == NULL || params == NULL) {
        return OST_PROTO_ERR_INVALID_ARGUMENT;
    }
    low = (uint16_t)(params->configured_size & 0xffffu);
    high = (uint16_t)(params->configured_size >> OST_PROTO_SIZE_PORTION_BITS);
    if (params->configured_size < OST_PROTO_MIN_CHUNK_DERIVATION_SIZE) {
        f2_argument = low;
        remainder_flag = 0u;
    } else {
        if (params->chunk_size == 0u) {
            return OST_PROTO_ERR_INVALID_ARGUMENT;
        }
        f2_argument = (uint16_t)(params->chunk_size & 0xffffu);
        remainder_flag = params->total_size % params->chunk_size != 0u ? 1u : 0u;
    }

    status = ost_proto_write_command(proto, OST_PROTO_CMD_SETUP_MODE, OST_PROTO_SETUP_MODE_ARG_FIRST);
    if (status == OST_PROTO_OK) {
        status = ost_proto_write_command(proto, OST_PROTO_CMD_SIZE_LOW, low);
    }
    if (status == OST_PROTO_OK) {
        status = ost_proto_write_command(proto, OST_PROTO_CMD_SIZE_HIGH, high);
    }
    if (status == OST_PROTO_OK) {
        status = ost_proto_write_command(proto, OST_PROTO_CMD_SETUP_MODE, OST_PROTO_SETUP_MODE_ARG_SECOND);
    }
    if (status == OST_PROTO_OK) {
        status = ost_proto_vendor_f2(proto, f2_argument);
    }
    if (status == OST_PROTO_OK) {
        status = ost_proto_write_command(proto, OST_PROTO_CMD_REMAINDER, remainder_flag);
    }
    return status;
}

int ost_proto_command_30(ost_proto *proto, uint16_t *value)
{
    return ost_proto_read_value(proto, OST_PROTO_CMD_INPUT_FLAG, value);
}

void ost_proto_set_worker_wait(ost_proto *proto, ost_proto_worker_wait wait, void *context)
{
    if (proto != NULL) {
        proto->worker_wait = wait;
        proto->worker_wait_context = context;
    }
}

int ost_proto_command_32(ost_proto *proto, uint16_t *value)
{
    int status;

    if (proto == NULL || value == NULL) {
        return OST_PROTO_ERR_INVALID_ARGUMENT;
    }
    status = ost_proto_vendor_f0(proto);
    if (status != OST_PROTO_OK) {
        return status;
    }
    status = ost_proto_write_command(proto, OST_PROTO_CMD_SETUP_MODE, OST_PROTO_SETUP_MODE_ARG_SECOND);
    if (status != OST_PROTO_OK) {
        return status;
    }
    if (proto->worker_wait != NULL) {
        status = proto->worker_wait(proto->worker_wait_context, OST_PROTO_WORKER_WAIT_TIMEOUT_MS);
        if (status != OST_PROTO_OK) {
            return status;
        }
    }
    return ost_proto_read_value(proto, OST_PROTO_CMD_INPUT_PREPARE, value);
}

int ost_proto_command_42(ost_proto *proto, uint16_t *value, unsigned int *error_flags)
{
    int status = ost_proto_read_value(proto, OST_PROTO_CMD_INPUT_OVERRUN, value);

    if (status == OST_PROTO_OK && error_flags != NULL) {
        *error_flags = (*value & OST_PROTO_RESPONSE_BIT_OVERRUN_OK) == 0u ? OST_PROTO_LOCAL_ERROR_OVERRUN : 0u;
    }
    return status;
}

void ost_proto_request_stop(ost_proto *proto)
{
    if (proto != NULL) {
        atomic_store(&proto->stop, 1);
    }
}

void ost_proto_set_close_hook(ost_proto *proto, ost_proto_close_hook hook, void *context)
{
    if (proto != NULL) {
        proto->close_hook = hook;
        proto->close_hook_context = context;
    }
}

static void notify_close(const ost_proto *proto, unsigned int step)
{
    if (proto->close_hook != NULL) {
        proto->close_hook(proto->close_hook_context, step);
    }
}

void ost_proto_close(ost_proto *proto)
{
    static const unsigned int pipe_order[OST_PROTO_PIPE_COUNT] = {
        OST_PROTO_PIPE_RESPONSE_IN,
        OST_PROTO_PIPE_COMMAND_OUT,
        OST_PROTO_PIPE_IMAGE_IN,
    };
    size_t i;

    if (proto == NULL) {
        return;
    }
    atomic_store(&proto->closed, 1);
    atomic_store(&proto->stop, 1);
    notify_close(proto, OST_PROTO_CLOSE_STEP_DEVICE);
    ost_close(proto->device);
    proto->device = NULL;
    for (i = 0u; i < OST_PROTO_PIPE_COUNT; ++i) {
        notify_close(proto, pipe_order[i]);
        proto->pipe_open[pipe_order[i]] = 0;
    }
    free(proto);
}
