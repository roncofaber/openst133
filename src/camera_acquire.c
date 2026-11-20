/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#include "camera_acquire.h"

#include "camera_modes.h"
#include "protocol.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUT_ENDPOINT 0x08u
#define IN_ENDPOINT 0x86u
#define IMAGE_ENDPOINT 0x82u
#define STARTUP_FIRST_URB 19968u
#define STARTUP_NEXT_URB 16384u
#define IMAGE_TIMEOUT_MARGIN_MS 10000u
#define POLL_LIMIT 200u
#define POLL_READY_BIT 0x80u
#define POLL_ECHO_VALUE 27u

#define CMD_FLAG 0x0030u
#define CMD_COUNT 0x0032u
#define CMD_SCALE 0x0034u
#define CMD_SHUTTER 0x003au
#define CMD_POLL 0x00e2u

#define OP_REQUEST 0x80u
#define OP_POLL 0xa0u
#define OP_EXPOSURE_COUNT 0xb0u
#define OP_EXPOSURE_SCALE 0xb1u
#define OP_SHUTTER 0xb2u
#define OP_VF2 0xc0u
#define OP_VF0 0xc1u
#define OP_IMAGE 0xc2u
#define OP_TAIL 0xc3u
/* A small OUT record, or (kind 2) a 0x20 table that is a slot of the startup data file. */
#define RECORD_TABLE 2u
typedef struct acquire_record {
    uint16_t command;
    uint16_t value;
    uint32_t count;
    uint32_t offset;
    uint8_t kind;
} acquire_record;

/* The acquisition operation table references variable large tables by startup-data slot. */
#define ACQUIRE_RECORD_COUNT 37
#define ACQUIRE_OP_COUNT 420
static const uint8_t k_request_commands[5] = {0x30, 0x32, 0x40, 0xe0, 0xe2};
static const acquire_record k_records[ACQUIRE_RECORD_COUNT] = {
    {0x0034, 0x0080, 0u, 0u, 0},
    {0x0030, 0x0002, 0u, 0u, 0},
    {0x0030, 0x0000, 0u, 0u, 0},
    {0x0030, 0x0001, 0u, 0u, 0},
    {0x0040, 0x0096, 0u, 0u, 0},
    {0x0040, 0x0095, 0u, 0u, 0},
    {0x0030, 0x0003, 0u, 0u, 0},
    {0x0040, 0x0015, 0u, 0u, 0},
    {0x0000, 0x0000, 0u, 0u, 0},
    {0x0022, 0x0000, 0u, 0u, 0},
    {0x0024, 0x0001, 0u, 0u, 0},
    {0x0020, 0u, 1368u, 1u, 2},
    {0x0022, 0x007e, 0u, 0u, 0},
    {0x0020, 0u, 512u, 2u, 2},
    {0x0042, 0x0036, 0u, 0u, 0},
    {0x0044, 0x0003, 0u, 0u, 0},
    {0x0050, 0x0080, 0u, 0u, 0},
    {0x0038, 0x0001, 0u, 0u, 0},
    {0x0036, 0x0000, 0u, 0u, 0},
    {0x003c, 0x0000, 0u, 0u, 0},
    {0x004a, 0x0000, 0u, 0u, 0},
    {0x004a, 0x0008, 0u, 0u, 0},
    {0x004a, 0u, 24u, 0u, 0},
    {0x004a, 0x000c, 0u, 0u, 0},
    {0x004a, 0x000e, 0u, 0u, 0},
    {0x004a, 0x000d, 0u, 0u, 0},
    {0x004a, 0x000f, 0u, 0u, 0},
    {0x004a, 0u, 24u, 24u, 0},
    {0x00fe, 0x0000, 0u, 0u, 0},
    {0x003c, 0x0001, 0u, 0u, 0},
    {0x0014, 0x0100, 0u, 0u, 0},
    {0x0010, 0x16e0, 0u, 0u, 0},
    {0x0012, 0x0004, 0u, 0u, 0},
    {0x0014, 0x0001, 0u, 0u, 0},
    {0x0016, 0x0001, 0u, 0u, 0},
    {0x00e0, 0x001b, 0u, 0u, 0},
    {0x00e2, 0x001b, 0u, 0u, 0},
};
static const uint8_t k_args[48] = {
    0x0c, 0x0e, 0x0c, 0x0c, 0x0e, 0x0c, 0x0c, 0x0e, 0x0c, 0x0c, 0x0e, 0x0c, 0x0c, 0x0e, 0x0c, 0x0d,
    0x0f, 0x0d, 0x0c, 0x0e, 0x0c, 0x0d, 0x0f, 0x0d, 0x0c, 0x0e, 0x0c, 0x0c, 0x0e, 0x0c, 0x0c, 0x0e,
    0x0c, 0x0c, 0x0e, 0x0c, 0x0c, 0x0e, 0x0c, 0x0d, 0x0f, 0x0d, 0x0c, 0x0e, 0x0c, 0x0c, 0x0e, 0x0c,
};
static const uint8_t k_ops[ACQUIRE_OP_COUNT] = {
    0x00, 0xb0, 0xb1, 0x80, 0x01, 0x02, 0x03, 0x04, 0x05, 0x02, 0x03, 0x06, 0x80, 0x01, 0x02, 0x03,
    0x04, 0x05, 0x02, 0x03, 0x06, 0x82, 0x07, 0x82, 0x05, 0x82, 0x08, 0x01, 0x02, 0x03, 0x82, 0x09,
    0x0a, 0x0b, 0x0c, 0x0a, 0x0d, 0x09, 0x0a, 0x0e, 0x0f, 0x04, 0x05, 0x10, 0x00, 0xb0, 0xb1, 0x11,
    0x12, 0xb2, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17,
    0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x19, 0x1a, 0x19, 0x17, 0x18,
    0x17, 0x19, 0x1a, 0x19, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17,
    0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x15, 0x14, 0x14, 0x15, 0x1b, 0x17, 0x18, 0x17, 0x17, 0x18,
    0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17,
    0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17,
    0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18,
    0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17,
    0x17, 0x18, 0x17, 0x15, 0x14, 0x02, 0x03, 0x06, 0x08, 0x1c, 0x13, 0x1d, 0x00, 0xb0, 0xb1, 0x80,
    0x01, 0x02, 0x03, 0x04, 0x05, 0x02, 0x03, 0x06, 0x80, 0x01, 0x02, 0x03, 0x04, 0x05, 0x02, 0x03,
    0x06, 0x82, 0x07, 0x82, 0x05, 0x82, 0x08, 0x01, 0x02, 0x03, 0x82, 0x09, 0x0a, 0x0b, 0x0c, 0x0a,
    0x0d, 0x09, 0x0a, 0x0e, 0x0f, 0x04, 0x05, 0x10, 0x00, 0xb0, 0xb1, 0x11, 0x12, 0xb2, 0x1d, 0x14,
    0x15, 0x16, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18,
    0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x19, 0x1a, 0x19, 0x17, 0x18, 0x17, 0x19, 0x1a, 0x19,
    0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17,
    0x18, 0x17, 0x15, 0x14, 0x14, 0x15, 0x1b, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17,
    0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17,
    0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18,
    0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17,
    0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x17, 0x18, 0x17, 0x15,
    0x14, 0x02, 0x03, 0x06, 0x08, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0xc0, 0x22, 0x09, 0x0a, 0xa0,
    0x23, 0x24, 0xa1, 0x83, 0xc1, 0x21, 0x81, 0xc2, 0xc3, 0x80, 0x01, 0x02, 0x03, 0x04, 0x05, 0x02,
    0x03, 0x06, 0x80, 0x01, 0x02, 0x03, 0x04, 0x05, 0x02, 0x03, 0x06, 0x80, 0x01, 0x02, 0x03, 0x04,
    0x05, 0x02, 0x03, 0x06,
};

typedef struct context {
    ost_device *device;
    int read_first;
    const ost_camera_mode *mode;
    const ost_camera_scan_program *scan;
    int rate_table_sent;
    int frame_count_set;
    uint16_t frame_count;
    unsigned int accumulations;
    char *error;
    size_t error_size;
} context;

static void fail(context *ctx, const char *format, ...)
{
    va_list arguments;

    if (ctx->error == NULL || ctx->error_size == 0u) {
        return;
    }
    va_start(arguments, format);
    (void)vsnprintf(ctx->error, ctx->error_size, format, arguments);
    va_end(arguments);
}

static int bulk_out(context *ctx, const unsigned char *data, size_t length)
{
    int moved = 0;
    int status = ost_bulk_transfer(
        ctx->device, OUT_ENDPOINT, (unsigned char *)(uintptr_t)data, (int)length,
        OST_PROTO_COMMAND_WRITE_TIMEOUT_MS, &moved);

    if (status != OST_SUCCESS || (size_t)moved != length) {
        fail(ctx, "bulk OUT of %zu bytes failed (%s, %d bytes moved)", length, ost_status_text(status), moved);
        return status != OST_SUCCESS ? status : OST_ERR_IO;
    }
    return OST_SUCCESS;
}

static int bulk_in(context *ctx, unsigned char response[OST_PROTO_RESPONSE_SIZE])
{
    int moved = 0;
    int status = ost_bulk_transfer(
        ctx->device, IN_ENDPOINT, response, (int)OST_PROTO_RESPONSE_SIZE,
        OST_PROTO_DEFAULT_RESPONSE_READ_TIMEOUT_MS, &moved);

    if (status != OST_SUCCESS || moved != (int)OST_PROTO_RESPONSE_SIZE) {
        fail(ctx, "two-byte response read failed (%s, %d bytes)", ost_status_text(status), moved);
        return status != OST_SUCCESS ? status : OST_ERR_IO;
    }
    return OST_SUCCESS;
}

static int request_value(context *ctx, uint16_t command, unsigned char *value)
{
    unsigned char request[OST_PROTO_SINGLE_COMMAND_SIZE];
    unsigned char response[OST_PROTO_RESPONSE_SIZE] = {0};
    int status;

    (void)ost_proto_encode_request(request, sizeof(request), command);
    if (ctx->read_first) {
        status = bulk_in(ctx, response);
        if (status == OST_SUCCESS) {
            status = bulk_out(ctx, request, sizeof(request));
        }
    } else {
        status = bulk_out(ctx, request, sizeof(request));
        if (status == OST_SUCCESS) {
            status = bulk_in(ctx, response);
        }
    }
    if (status != OST_SUCCESS) {
        return status;
    }
    if (response[0] != response[1]) {
        fail(ctx, "response to request 0x%02x is %02x %02x; expected two identical bytes",
            (unsigned int)command, (unsigned int)response[0], (unsigned int)response[1]);
        return OST_ERR_IO;
    }
    *value = response[0];
    return OST_SUCCESS;
}

static int write_single(context *ctx, uint16_t command, uint16_t argument)
{
    unsigned char record[OST_PROTO_SINGLE_COMMAND_SIZE];

    (void)ost_proto_encode_command(record, sizeof(record), command, argument);
    return bulk_out(ctx, record, sizeof(record));
}

static int write_record(context *ctx, const ost_startup_data *data, const acquire_record *record)
{
    unsigned char *arguments;
    unsigned char *wire;
    size_t count = record->count;
    size_t size;
    size_t done = 0u;
    uint16_t value = record->value;
    int status = OST_SUCCESS;

    if (record->command == 0x44u) {
        value = ctx->mode->readout_44;
    } else if (record->command == 0x50u) {
        value = ctx->mode->readout_50;
    } else if (record->command == 0x10u) {
        value = (uint16_t)(ctx->mode->frame_bytes & 0xffffu);
    } else if (record->command == 0x12u) {
        value = (uint16_t)(ctx->mode->frame_bytes >> 16u);
    } else if (record->command == 0x3cu && record->value == 1u && ctx->frame_count_set) {
        value = ctx->frame_count;
    }
    if (record->count == 0u) {
        return write_single(ctx, record->command, value);
    }
    if (record->kind == RECORD_TABLE) {
        const unsigned char *table = NULL;
        uint16_t command = 0u;
        size_t slot = record->command == 0x20u && record->offset == 1u ? ctx->mode->scan_table_slot : record->offset;

        if (record->command == 0x20u && record->offset == 1u && ctx->scan != NULL) {
            table = ctx->scan->arguments;
            count = ctx->scan->count;
            command = 0x20u;
        } else if (data != NULL) {
            table = ost_startup_data_table(data, slot, &command, &count);
        }
        if (table == NULL || command != record->command) {
            fail(ctx, "the 0x%02x configuration table for this mode is missing from the startup data file; build it from "
                "your licensed WinSpec installation with openst133 startup-data",
                (unsigned int)record->command);
            return OST_ERR_UNSUPPORTED;
        }
        arguments = malloc(count);
        size = 3u * count + 3u;
        wire = malloc(size);
        if (arguments == NULL || wire == NULL) {
            free(arguments);
            free(wire);
            fail(ctx, "out of memory for a %zu-byte record", size);
            return OST_ERR_NO_MEMORY;
        }
        memcpy(arguments, table, count);
    } else {
        arguments = malloc(count);
        size = 3u * count + 3u;
        wire = malloc(size);
        if (arguments == NULL || wire == NULL) {
            free(arguments);
            free(wire);
            fail(ctx, "out of memory for a %zu-byte record", size);
            return OST_ERR_NO_MEMORY;
        }
        memcpy(arguments, k_args + record->offset, count);
    }
    if (ost_proto_encode_command_bytes(wire, size, record->command, arguments, count) != size) {
        fail(ctx, "configuration data for command 0x%02x is inconsistent", (unsigned int)record->command);
        status = OST_ERR_IO;
    }
    while (status == OST_SUCCESS && done < size) {
        size_t limit = done == 0u ? STARTUP_FIRST_URB : STARTUP_NEXT_URB;
        size_t chunk = size - done < limit ? size - done : limit;

        status = bulk_out(ctx, wire + done, chunk);
        done += chunk;
    }
    free(arguments);
    free(wire);
    return status;
}

static int vendor_out(context *ctx, uint8_t request, uint16_t value)
{
    unsigned char zeros[8] = {0};
    int moved = 0;
    int status = ost_control_transfer(
        ctx->device, 0x40u, request, value, 0u, zeros, sizeof(zeros), OST_PROTO_DEFAULT_CONTROL_TIMEOUT_MS, &moved);

    if (status != OST_SUCCESS || moved != (int)sizeof(zeros)) {
        fail(ctx, "vendor request 0x%02x failed (%s, %d bytes)", (unsigned int)request, ost_status_text(status), moved);
        return status != OST_SUCCESS ? status : OST_ERR_IO;
    }
    return OST_SUCCESS;
}

static uint8_t *expand_ops(void)
{
    uint8_t *ops = malloc(ACQUIRE_OP_COUNT);

    if (ops != NULL) {
        memcpy(ops, k_ops, ACQUIRE_OP_COUNT);
    }
    return ops;
}

/* Read one frame from the image request partition; on abort done holds the bytes stored. */
static int read_frame(context *ctx, const ost_acquire_options *options, unsigned char *frame, size_t *received,
    int *aborted)
{
    size_t done = 0u;
    size_t read = 0u;
    int status = OST_SUCCESS;

    while (done < ctx->mode->frame_bytes && status == OST_SUCCESS) {
        size_t remaining = ctx->mode->frame_bytes - done;
        size_t want;
        int moved = 0;

        if (options->abort_requested != NULL && atomic_load(options->abort_requested) != 0) {
            *received = done;
            *aborted = 1;
            return OST_SUCCESS;
        }
        want = read == 0u ? (remaining < STARTUP_FIRST_URB ? remaining : STARTUP_FIRST_URB)
                          : (remaining < STARTUP_NEXT_URB ? remaining : STARTUP_NEXT_URB);
        status = ost_bulk_transfer(ctx->device, IMAGE_ENDPOINT, frame + done, (int)want,
            (read == 0u ? options->exposure_ms : 0u) + IMAGE_TIMEOUT_MARGIN_MS, &moved);
        if (status != OST_SUCCESS || (size_t)moved != want) {
            if (moved > 0 && (size_t)moved <= want) {
                done += (size_t)moved;
            }
            fail(ctx, "image read %zu returned %d of %zu bytes (%s); the frame is incomplete", read + 1u,
                moved, want, ost_status_text(status));
            if (status == OST_SUCCESS) {
                status = OST_ERR_IO;
            }
            break;
        }
        done += (size_t)moved;
        ++read;
    }
    *received = done;
    return status;
}

/*
 * The camera returns ctx->accumulations complete frames back to back. Store their per-pixel sum as
 * little-endian int32, the SPE LONG data type. received stays 0 unless every frame arrived.
 */
static int read_accumulated(context *ctx, const ost_acquire_options *options, unsigned char *sum, size_t *received,
    int *aborted)
{
    size_t pixels = ctx->mode->frame_bytes / OST_CAMERA_BYTES_PER_PIXEL;
    unsigned char *frame = malloc(ctx->mode->frame_bytes);
    unsigned int index;
    size_t got = 0u;
    size_t p;
    int status = OST_SUCCESS;

    *received = 0u;
    if (frame == NULL) {
        fail(ctx, "out of memory for a %zu-byte frame", ctx->mode->frame_bytes);
        return OST_ERR_NO_MEMORY;
    }
    memset(sum, 0, pixels * sizeof(int32_t));
    for (index = 0u; index < ctx->accumulations && status == OST_SUCCESS; ++index) {
        status = read_frame(ctx, options, frame, &got, aborted);
        if (status != OST_SUCCESS && ctx->error != NULL && ctx->error_size != 0u) {
            char detail[192];

            (void)snprintf(detail, sizeof(detail), "%s", ctx->error);
            fail(ctx, "accumulated frame %u of %u: %s", index + 1u, (unsigned int)ctx->accumulations, detail);
        }
        if (status != OST_SUCCESS || *aborted) {
            break;
        }
        for (p = 0u; p < pixels; ++p) {
            uint32_t value = (uint32_t)frame[2u * p] | ((uint32_t)frame[2u * p + 1u] << 8u);
            uint32_t total = ((uint32_t)sum[4u * p] | ((uint32_t)sum[4u * p + 1u] << 8u) |
                ((uint32_t)sum[4u * p + 2u] << 16u) | ((uint32_t)sum[4u * p + 3u] << 24u)) + value;

            sum[4u * p] = (unsigned char)(total & 0xffu);
            sum[4u * p + 1u] = (unsigned char)((total >> 8u) & 0xffu);
            sum[4u * p + 2u] = (unsigned char)((total >> 16u) & 0xffu);
            sum[4u * p + 3u] = (unsigned char)(total >> 24u);
        }
    }
    free(frame);
    if (status == OST_SUCCESS && !*aborted) {
        *received = pixels * sizeof(int32_t);
    }
    return status;
}

/*
 * Replace the 16-bit controller gain word in every copy of the first command 0x4a serial group: a 24-argument 0x4a
 * record (an 8-bit prefix) followed by 16 single-write triplets, 0c 0e 0c for 0 and 0d 0f 0d for 1, most significant
 * bit first. The operation template must hold the startup gain 2 word at every copy.
 */
static int apply_gain_word(context *ctx, uint8_t *ops, uint16_t word)
{
    static const uint16_t triplet[2][3] = {{0x000cu, 0x000eu, 0x000cu}, {0x000du, 0x000fu, 0x000du}};
    uint8_t index[2][3];
    uint8_t prefix = ACQUIRE_RECORD_COUNT;
    size_t copies = 0u;
    size_t i;
    unsigned int bit;
    unsigned int k;
    uint8_t r;

    for (r = 0u; r < ACQUIRE_RECORD_COUNT; ++r) {
        if (k_records[r].command == 0x4au && k_records[r].count == 24u && k_records[r].offset == 0u) {
            prefix = r;
        }
    }
    for (bit = 0u; bit < 2u; ++bit) {
        for (k = 0u; k < 3u; ++k) {
            index[bit][k] = ACQUIRE_RECORD_COUNT;
            for (r = 0u; r < ACQUIRE_RECORD_COUNT; ++r) {
                if (k_records[r].command == 0x4au && k_records[r].count == 0u &&
                    k_records[r].value == triplet[bit][k]) {
                    index[bit][k] = r;
                }
            }
        }
    }
    for (i = 0u; prefix < ACQUIRE_RECORD_COUNT && i + 48u < ACQUIRE_OP_COUNT; ++i) {
        if (ops[i] != prefix) {
            continue;
        }
        for (bit = 0u; bit < 16u; ++bit) {
            unsigned int initial = (OST_SETTINGS_GAIN_WORD_2 >> (15u - bit)) & 1u;
            unsigned int wanted = ((unsigned int)word >> (15u - bit)) & 1u;

            for (k = 0u; k < 3u; ++k) {
                if (index[initial][k] >= ACQUIRE_RECORD_COUNT || ops[i + 1u + 3u * bit + k] != index[initial][k]) {
                    fail(ctx, "the acquisition command 0x4a gain group does not hold the startup gain 2 word; the "
                        "controller gain cannot be set");
                    return OST_ERR_UNSUPPORTED;
                }
                ops[i + 1u + 3u * bit + k] = index[wanted][k];
            }
        }
        ++copies;
        i += 48u;
    }
    if (copies == 0u) {
        fail(ctx, "the acquisition sequence holds no command 0x4a gain group; the controller gain cannot be set");
        return OST_ERR_UNSUPPORTED;
    }
    return OST_SUCCESS;
}

static int run_ops(context *ctx, const ost_acquire_options *options, const uint8_t *ops, size_t first, size_t last,
    unsigned int count, unsigned int scale, unsigned char *frame, size_t *received, int *aborted)
{
    size_t i;
    int status = OST_SUCCESS;

    for (i = first; i < last && status == OST_SUCCESS; ++i) {
        uint8_t op = ops[i];

        if (op < OP_REQUEST) {
            if (op >= ACQUIRE_RECORD_COUNT) {
                fail(ctx, "operation %zu names a missing record", i);
                return OST_ERR_IO;
            }
            if (op == 9u && ctx->mode->rate_table && !ctx->rate_table_sent) {
                static const acquire_record rate_record = {0x0048, 0u, 32768u, 7u, RECORD_TABLE};

                status = write_single(ctx, 0x0040u, 0x0096u);
                if (status == OST_SUCCESS) {
                    status = write_single(ctx, 0x0040u, 0x0097u);
                }
                if (status == OST_SUCCESS) {
                    status = write_record(ctx, options->data, &rate_record);
                }
                ctx->rate_table_sent = 1;
            }
            if (status == OST_SUCCESS) {
                status = write_record(ctx, options->data, &k_records[op]);
            }
        } else if (op < OP_POLL) {
            unsigned char value = 0u;

            if (op - OP_REQUEST >= sizeof(k_request_commands)) {
                fail(ctx, "operation %zu names a missing request", i);
                return OST_ERR_IO;
            }
            status = request_value(ctx, k_request_commands[op - OP_REQUEST], &value);
        } else if (op == OP_POLL || op == OP_POLL + 1u) {
            unsigned int n;

            for (n = 0u; n < POLL_LIMIT && status == OST_SUCCESS; ++n) {
                unsigned char value = 0u;

                status = request_value(ctx, CMD_POLL, &value);
                if (status == OST_SUCCESS &&
                    (op == OP_POLL ? (value & POLL_READY_BIT) != 0u
                                   : (uint8_t)(value - POLL_READY_BIT) == POLL_ECHO_VALUE)) {
                    break;
                }
            }
        } else if (op == OP_EXPOSURE_COUNT) {
            status = write_single(ctx, CMD_COUNT, (uint16_t)count);
        } else if (op == OP_EXPOSURE_SCALE) {
            status = write_single(ctx, CMD_SCALE, (uint16_t)scale);
        } else if (op == OP_SHUTTER) {
            status = write_single(ctx, CMD_SHUTTER, options->plan->reg_3a);
        } else if (op == OP_VF2) {
            status = vendor_out(ctx, 0xf2u, 0x0200u);
        } else if (op == OP_VF0) {
            status = vendor_out(ctx, 0xf0u, 0u);
        } else if (op == OP_IMAGE) {
            if (ctx->accumulations <= 1u) {
                status = read_frame(ctx, options, frame, received, aborted);
            } else {
                status = read_accumulated(ctx, options, frame, received, aborted);
            }
            if (*aborted) {
                return OST_SUCCESS;
            }
        } else if (op == OP_TAIL) {
            return OST_SUCCESS;
        } else {
            fail(ctx, "operation %zu has an unknown code 0x%02x", i, (unsigned int)op);
            return OST_ERR_IO;
        }
    }
    return status;
}

int ost_acquire_run(
    ost_device *device,
    const ost_acquire_options *options,
    unsigned char *frame,
    size_t capacity,
    size_t *received,
    char *error,
    size_t error_size)
{
    context ctx;
    ost_camera_mode mode;
    unsigned int count = 0u;
    unsigned int scale = 0u;
    uint8_t *ops;
    size_t tail = 0u;
    size_t group_end;
    size_t i;
    int aborted = 0;
    int status;

    if (device == NULL || options == NULL || options->plan == NULL || frame == NULL || received == NULL) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    *received = 0u;
    ctx.device = device;
    ctx.read_first = options->read_first;
    ctx.mode = &mode;
    ctx.scan = &options->plan->scan;
    ctx.rate_table_sent = 0;
    ctx.frame_count_set = options->frame_count_set;
    ctx.frame_count = options->frame_count;
    ctx.accumulations = 1u;
    ctx.error = error;
    ctx.error_size = error_size;
    if (error != NULL && error_size != 0u) {
        error[0] = '\0';
    }
    memset(&mode, 0, sizeof(mode));
    mode.frame_bytes = options->plan->frame_bytes;
    mode.scan_table_slot = options->plan->scan_table_slot;
    mode.readout_44 = options->plan->reg_44;
    mode.readout_50 = options->plan->reg_50;
    mode.rate_table = options->plan->insert_rate_table;
    count = options->plan->exposure_count;
    scale = options->plan->exposure_scale;
    if (options->plan->accumulations > 1u) {
        ctx.accumulations = options->plan->accumulations;
        ctx.frame_count_set = 1;
        ctx.frame_count = options->plan->accumulations;
    }
    if (capacity < (ctx.accumulations > 1u ? options->plan->output_bytes : mode.frame_bytes)) {
        fail(&ctx, "buffer holds %zu bytes but the selected frame needs %zu", capacity,
            ctx.accumulations > 1u ? options->plan->output_bytes : mode.frame_bytes);
        return OST_ERR_BUFFER_TOO_SMALL;
    }
    if (options->data == NULL) {
        fail(&ctx, "acquisition needs the startup data file (docs/user/startup-data.md)");
        return OST_ERR_UNSUPPORTED;
    }
    ops = expand_ops();
    if (ops == NULL) {
        fail(&ctx, "the acquisition operation stream is damaged");
        return OST_ERR_IO;
    }
    if (options->plan->gain_word != 0u) {
        status = apply_gain_word(&ctx, ops, options->plan->gain_word);
        if (status != OST_SUCCESS) {
            free(ops);
            return status;
        }
    }
    for (i = 0u; i < ACQUIRE_OP_COUNT; ++i) {
        if (ops[i] == OP_TAIL) {
            tail = i;
            break;
        }
    }
    status = run_ops(&ctx, options, ops, 0u, tail, count, scale, frame, received, &aborted);
    if (status == OST_SUCCESS && !aborted) {
        status = run_ops(&ctx, options, ops, tail + 1u, ACQUIRE_OP_COUNT, count, scale, frame, received, &aborted);
    } else if (status == OST_SUCCESS && aborted) {
        group_end = tail + 2u;
        while (group_end < ACQUIRE_OP_COUNT && ops[group_end] != OP_REQUEST) {
            ++group_end;
        }
        status = run_ops(&ctx, options, ops, tail + 1u, group_end, count, scale, frame, received, &aborted);
        if (status == OST_SUCCESS) {
            fail(&ctx, "acquisition aborted after %zu bytes; the camera stop exchange was sent", *received);
            status = OST_ERR_ABORTED;
        }
    }
    free(ops);
    return status;
}
