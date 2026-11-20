/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#include "camera_recover.h"

#include "camera_startup.h"
#include "protocol.h"
#include "transport_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define OUT_ENDPOINT 0x08u
#define IN_ENDPOINT 0x86u
#define IMAGE_ENDPOINT 0x82u
#define IMAGE_DRAIN_BYTES 20480u
#define IMAGE_DRAIN_TIMEOUT_MS 25u
#define IMAGE_DRAIN_MAX_READS 64u
#define CMD_FLAG 0x0030u
#define CMD_PORT 0x0040u
#define CMD_STATUS 0x0042u

typedef struct recovery_context {
    ost_device *device;
    int read_first;
    char *error;
    size_t error_size;
} recovery_context;

static void fail(recovery_context *context, const char *format, ...)
{
    va_list arguments;

    if (context->error == NULL || context->error_size == 0u) {
        return;
    }
    va_start(arguments, format);
    (void)vsnprintf(context->error, context->error_size, format, arguments);
    va_end(arguments);
}

static int bulk_out(recovery_context *context, const unsigned char *data, size_t length)
{
    int moved = 0;
    int status = ost_bulk_transfer(context->device, OUT_ENDPOINT, (unsigned char *)(uintptr_t)data,
        (int)length, OST_PROTO_COMMAND_WRITE_TIMEOUT_MS, &moved);

    if (status != OST_SUCCESS || moved != (int)length) {
        fail(context, "recovery bulk OUT of %zu bytes failed (%s, %d bytes moved)", length,
            ost_status_text(status), moved);
        return status != OST_SUCCESS ? status : OST_ERR_IO;
    }
    return OST_SUCCESS;
}

static int bulk_in(recovery_context *context, unsigned char response[OST_PROTO_RESPONSE_SIZE])
{
    int moved = 0;
    int status = ost_bulk_transfer(context->device, IN_ENDPOINT, response, OST_PROTO_RESPONSE_SIZE,
        OST_PROTO_DEFAULT_RESPONSE_READ_TIMEOUT_MS, &moved);

    if (status != OST_SUCCESS || moved != (int)OST_PROTO_RESPONSE_SIZE) {
        fail(context, "recovery response read failed (%s, %d bytes)", ost_status_text(status), moved);
        return status != OST_SUCCESS ? status : OST_ERR_IO;
    }
    return OST_SUCCESS;
}

static int request_value(recovery_context *context, uint16_t command, unsigned char *value)
{
    unsigned char request[OST_PROTO_SINGLE_COMMAND_SIZE];
    unsigned char response[OST_PROTO_RESPONSE_SIZE] = {0};
    int status;

    (void)ost_proto_encode_request(request, sizeof(request), command);
    if (context->read_first) {
        status = bulk_in(context, response);
        if (status == OST_SUCCESS) {
            status = bulk_out(context, request, sizeof(request));
        }
    } else {
        status = bulk_out(context, request, sizeof(request));
        if (status == OST_SUCCESS) {
            status = bulk_in(context, response);
        }
    }
    if (status != OST_SUCCESS) {
        return status;
    }
    if (response[0] != response[1]) {
        fail(context, "recovery request 0x%02x answered %02x %02x; valid responses repeat one byte",
            (unsigned int)command, response[0], response[1]);
        return OST_ERR_IO;
    }
    *value = response[0];
    return OST_SUCCESS;
}

static int write_single(recovery_context *context, uint16_t command, uint16_t argument)
{
    unsigned char record[OST_PROTO_SINGLE_COMMAND_SIZE];

    (void)ost_proto_encode_command(record, sizeof(record), command, argument);
    return bulk_out(context, record, sizeof(record));
}

static int drain_images(recovery_context *context)
{
    unsigned char data[IMAGE_DRAIN_BYTES];
    unsigned int read;

    for (read = 0u; read < IMAGE_DRAIN_MAX_READS; ++read) {
        int moved = 0;
        int status = ost_bulk_transfer(context->device, IMAGE_ENDPOINT, data, sizeof(data),
            IMAGE_DRAIN_TIMEOUT_MS, &moved);

        if (status == OST_TRANSPORT_ERR_TIMEOUT || status == OST_ERR_TIMEOUT) {
            return OST_SUCCESS;
        }
        if (status != OST_SUCCESS || moved < 0 || (size_t)moved > sizeof(data)) {
            fail(context, "image drain after status 0x%02x failed (%s, %d bytes)",
                OST_CAMERA_STATUS_ACQUIRING, ost_status_text(status), moved);
            return status != OST_SUCCESS ? status : OST_ERR_IO;
        }
        if (moved == 0) {
            return OST_SUCCESS;
        }
    }
    fail(context, "image drain after status 0x%02x still returned data after %u reads",
        OST_CAMERA_STATUS_ACQUIRING, IMAGE_DRAIN_MAX_READS);
    return OST_ERR_IO;
}

static int send_camera_stop(recovery_context *context)
{
    static const uint16_t writes[][2] = {
        {CMD_FLAG, 2u}, {CMD_FLAG, 0u}, {CMD_FLAG, 1u}, {CMD_PORT, 150u},
        {CMD_PORT, 149u}, {CMD_FLAG, 0u}, {CMD_FLAG, 1u}, {CMD_FLAG, 3u}
    };
    unsigned char flag = 0u;
    size_t i;
    int status = request_value(context, CMD_FLAG, &flag);

    if (status != OST_SUCCESS) {
        return status;
    }
    if (flag != 0u) {
        fail(context, "stop request 0x%02x answered 0x%02x, expected 0x00",
            (unsigned int)CMD_FLAG, (unsigned int)flag);
        return OST_ERR_IO;
    }
    for (i = 0u; i < sizeof(writes) / sizeof(writes[0]); ++i) {
        status = write_single(context, writes[i][0], writes[i][1]);
        if (status != OST_SUCCESS) {
            return status;
        }
    }
    return OST_SUCCESS;
}

int ost_camera_recover(
    ost_device *device,
    int read_first,
    unsigned char observed,
    unsigned char *status,
    char *error,
    size_t error_size)
{
    recovery_context context;
    int result;

    if (device == NULL || status == NULL) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    context.device = device;
    context.read_first = read_first;
    context.error = error;
    context.error_size = error_size;
    if (error != NULL && error_size != 0u) {
        error[0] = '\0';
    }
    *status = observed;
    if (observed != OST_CAMERA_STATUS_ACQUIRING) {
        return OST_SUCCESS;
    }
    result = drain_images(&context);
    if (result == OST_SUCCESS) {
        result = send_camera_stop(&context);
    }
    if (result == OST_SUCCESS) {
        result = request_value(&context, CMD_STATUS, status);
    }
    if (result == OST_SUCCESS && *status == OST_CAMERA_STATUS_ACQUIRING) {
        fail(&context, "status request answered 0x%02x after acquisition recovery, expected 0x%02x",
            (unsigned int)*status, OST_STARTUP_STATUS_LIVE);
        return OST_ERR_IO;
    }
    return result;
}
