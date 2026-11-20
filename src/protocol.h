/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#ifndef OPENST133_PROTOCOL_H
#define OPENST133_PROTOCOL_H

#include "openst133.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Generic USB physical-port protocol used by the ST-133 controller.
 */

/** Status codes of the protocol layer; transport errors are mapped, never passed through. */
typedef enum ost_proto_status {
    OST_PROTO_OK = 0,
    OST_PROTO_ERR_INVALID_ARGUMENT = -1,
    OST_PROTO_ERR_NO_MEMORY = -2,
    OST_PROTO_ERR_TRANSPORT = -3,
    OST_PROTO_ERR_NO_DEVICE = -4,
    OST_PROTO_ERR_UNSUPPORTED = -5,
    OST_PROTO_ERR_TIMEOUT = -6,
    OST_PROTO_ERR_SHORT_TRANSFER = -7,
    OST_PROTO_ERR_CLOSED = -8,
    OST_PROTO_ERR_STOPPED = -9
} ost_proto_status;

/* Zero-based logical pipe selections; these are not endpoint addresses. */
#define OST_PROTO_PIPE_IMAGE_IN 0u
#define OST_PROTO_PIPE_RESPONSE_IN 1u
#define OST_PROTO_PIPE_COMMAND_OUT 2u
#define OST_PROTO_PIPE_COUNT 3u

/*
 * Candidate endpoint addresses pipes 0, 1 and 2 in descriptor order under
 * alternate setting 0. The endpoint mapping is configurable in ost_proto_config.
 */
#define OST_PROTO_CANDIDATE_ENDPOINT_PIPE0 0x82u
#define OST_PROTO_CANDIDATE_ENDPOINT_PIPE1 0x86u
#define OST_PROTO_CANDIDATE_ENDPOINT_PIPE2 0x08u

/* Record-layout constants. */
#define OST_PROTO_RECORD_LEAD 0x01u
#define OST_PROTO_RECORD_ARG_TAG 0x02u
#define OST_PROTO_RECORD_REQUEST_TAG 0x03u
#define OST_PROTO_SINGLE_COMMAND_SIZE 6u
#define OST_PROTO_RESPONSE_SIZE 2u

/*
 * Bounded control and descriptor transfer timeout.
 */
#define OST_PROTO_DEFAULT_CONTROL_TIMEOUT_MS 5000u

/* Command and image transfer timeouts in milliseconds. */
#define OST_PROTO_COMMAND_WRITE_TIMEOUT_MS 10000u
#define OST_PROTO_IMAGE_TIMEOUT_DIVISOR 2000u
#define OST_PROTO_IMAGE_TIMEOUT_BASE_MS 1000u

/*
 * The protocol gives the multi-argument write timeout as 0xffffffff, which is
 * unbounded in practice. The layer never blocks forever, so the sentinel
 * is kept for reference only and the effective value is
 * ost_proto_config.multi_write_timeout_ms.
 */
#define OST_PROTO_NO_MULTI_WRITE_TIMEOUT 0xffffffffu

/*
 * Response-read timing: the timeout of the two-byte
 * read that precedes every single-input request uses a bounded default. The default
 * reuses the 10000 ms local-event wait of the same operation.
 */
#define OST_PROTO_DEFAULT_RESPONSE_READ_TIMEOUT_MS 10000u

/* Transfer-size setup command numbers. */
#define OST_PROTO_CMD_SETUP_MODE 0x0014u
#define OST_PROTO_CMD_SIZE_LOW 0x0010u
#define OST_PROTO_CMD_SIZE_HIGH 0x0012u
#define OST_PROTO_CMD_REMAINDER 0x0016u
#define OST_PROTO_SETUP_MODE_ARG_FIRST 0x0100u
#define OST_PROTO_SETUP_MODE_ARG_SECOND 0x0001u

/*
 * below this configured size request 0xf2
 * takes the size itself and command 0x0016 takes 0; from it upward 0xf2 takes
 * the chunk size and command 0x0016 reflects the remainder.
 */
#define OST_PROTO_MIN_CHUNK_DERIVATION_SIZE 0x200u

/* Single-input commands with their own command sequences. */
#define OST_PROTO_CMD_INPUT_FLAG 0x0030u
#define OST_PROTO_CMD_INPUT_PREPARE 0x0032u
#define OST_PROTO_CMD_INPUT_OVERRUN 0x0042u

/* wait on the input-worker notification event before the 0x32 request. */
#define OST_PROTO_WORKER_WAIT_TIMEOUT_MS 5000u

/*
 * The caller tests bit 1 of the 0x42 response; a clear bit sets
 * this local error bit. */
#define OST_PROTO_RESPONSE_BIT_OVERRUN_OK 0x0002u
#define OST_PROTO_LOCAL_ERROR_OVERRUN 0x80u

/*
 * Transfer-size fields: width of the low and high portions of the
 * configured transfer size. Sixteen bits each is the only width that fits the
 * 16-bit command argument; both portions match the command-argument width.
 */
#define OST_PROTO_SIZE_PORTION_BITS 16u

/* Vendor requests; selector class 2 (vendor), recipient 0 (device). */
#define OST_PROTO_VENDOR_REQ_F0 0xf0u
#define OST_PROTO_VENDOR_REQ_F2 0xf2u
#define OST_PROTO_VENDOR_REQ_F1 0xf1u
#define OST_PROTO_VENDOR_REQ_F3 0xf3u
#define OST_PROTO_VENDOR_F1_IN_SIZE 2u
#define OST_PROTO_VENDOR_OUT_SIZE 8u
#define OST_PROTO_VENDOR_IN_SIZE 4u

/*
 * USB setup layout: the two setup arguments of the
 * generic selector land in the USB setup packet. They are placed in wValue and
 * wIndex in order; the request types below follow from direction, vendor
 * class and device recipient.
 */
#define OST_PROTO_VENDOR_REQTYPE_OUT 0x40u
#define OST_PROTO_VENDOR_REQTYPE_IN 0xc0u

/** Pipe map and timeouts; start from ost_proto_config_default. */
typedef struct ost_proto_config {
    unsigned char pipe_endpoint[OST_PROTO_PIPE_COUNT];
    unsigned int command_write_timeout_ms;
    unsigned int multi_write_timeout_ms;
    unsigned int response_read_timeout_ms;
    unsigned int control_timeout_ms;
} ost_proto_config;

/** Parameters of the transfer-size setup. */
typedef struct ost_proto_transfer_params {
    uint32_t configured_size;
    uint32_t chunk_size;
    uint32_t total_size;
} ost_proto_transfer_params;

typedef struct ost_proto ost_proto;

/** Fill config with the candidate pipe map and the default timeouts. */
void ost_proto_config_default(ost_proto_config *config);

/** Map a transport status to a protocol status. */
int ost_proto_from_transport_status(int transport_status);

/** Text for a protocol status; never NULL. */
const char *ost_proto_status_text(int status);

/*
 * Pure encoders. They return the record size, or 0 when capacity is too small
 * or an argument is invalid; nothing is written on failure.
 */
size_t ost_proto_encode_command(unsigned char *out, size_t capacity, uint16_t command, uint16_t argument);
size_t ost_proto_encode_command_bytes(
    unsigned char *out,
    size_t capacity,
    uint16_t command,
    const unsigned char *arguments,
    size_t argument_count);
size_t ost_proto_encode_request(unsigned char *out, size_t capacity, uint16_t command);
uint16_t ost_proto_parse_response(const unsigned char response[OST_PROTO_RESPONSE_SIZE]);

/** Open the ST-133 at info through transport (NULL selects libusb) and own the device. */
int ost_proto_open(
    const ost_transport *transport,
    const ost_device_info *info,
    const ost_proto_config *config,
    ost_proto **proto);

/*
 * Open pipes 1, 2 and 0 in that order. Repeating the call is a
 * no-op because already-open state suppresses repeated opens.
 */
int ost_proto_open_pipes(ost_proto *proto);

/** Single command and 16-bit argument: six bytes on pipe 2. */
int ost_proto_write_command(ost_proto *proto, uint16_t command, uint16_t argument);

/** Command with one byte per argument triplet, 3N+3 bytes on pipe 2. */
int ost_proto_write_command_bytes(
    ost_proto *proto,
    uint16_t command,
    const unsigned char *arguments,
    size_t argument_count);

/** Ordinary single input: two-byte read, six-byte request; value is (R1 << 8) | R0. */
int ost_proto_read_value(ost_proto *proto, uint16_t command, uint16_t *value);

/** Bulk image input: chunked reads on pipe 0 until total bytes arrive. */
int ost_proto_read_image(ost_proto *proto, unsigned char *buffer, size_t total, size_t chunk_size, size_t *received);

/** Vendor request 0xf0. */
int ost_proto_vendor_f0(ost_proto *proto);
/** Vendor request 0xf2 with a caller-supplied first setup argument. */
int ost_proto_vendor_f2(ost_proto *proto, uint16_t argument);
/** Vendor request 0xf1 returning two bytes, decoded R0 + 256 * R1 (16-bit little-endian value). */
int ost_proto_vendor_f1(ost_proto *proto, uint16_t *value);
/** Vendor request 0xf3 returning four bytes. */
int ost_proto_vendor_f3(ost_proto *proto, unsigned char response[OST_PROTO_VENDOR_IN_SIZE]);

/*
 * Transfer-size setup: commands 0x14(0x0100),
 * 0x10, 0x12, 0x14(0x0001), vendor request 0xf2, command 0x16. Below 0x200
 * configured bytes the 0xf2 argument is configured_size & 0xffff, command 0x16
 * carries 0 and chunk_size and total_size are ignored. From 0x200 upward the
 * 0xf2 argument is chunk_size & 0xffff (chunk_size must be nonzero) and command
 * 0x16 carries 1 when total_size is not a multiple of chunk_size.
 */
int ost_proto_setup_transfer(ost_proto *proto, const ost_proto_transfer_params *params);

/*
 * Single-input command 0x30: the ordinary request sequence; the
 * local input-state flag it clears has no wire effect.
 */
int ost_proto_command_30(ost_proto *proto, uint16_t *value);

/*
 * Optional wait on the input-worker notification event of command 0x32.
 * The layer has no worker of its own, so without a hook the
 * step is skipped. The hook gets OST_PROTO_WORKER_WAIT_TIMEOUT_MS and returns
 * an ost_proto_status; a nonzero result aborts the command.
 */
typedef int (*ost_proto_worker_wait)(void *context, unsigned int timeout_ms);
void ost_proto_set_worker_wait(ost_proto *proto, ost_proto_worker_wait wait, void *context);

/*
 * Command 0x32: vendor 0xf0, record 01 14 00 02 01 00, worker
 * wait, then the ordinary 0x32 request sequence.
 */
int ost_proto_command_32(ost_proto *proto, uint16_t *value);

/*
 * Command 0x42: ordinary sequence; *error_flags receives
 * OST_PROTO_LOCAL_ERROR_OVERRUN when bit 1 of the response is clear, else 0.
 * error_flags may be NULL.
 */
int ost_proto_command_42(ost_proto *proto, uint16_t *value, unsigned int *error_flags);

/*
 * Completion check of multiple input, zero-time and without a pipe
 * read: after ost_proto_read_image completed successfully it returns the
 * accumulated completed byte count once and clears it; otherwise *completed is 0.
 */
int ost_proto_multiple_input_take(ost_proto *proto, size_t *completed);

/** Ask a running ost_proto_read_image to stop between chunks; safe from another thread. */
void ost_proto_request_stop(ost_proto *proto);

/*
 * Close: stop new submissions, then close the device handle, then
 * pipes 1, 2 and 0. No shutdown command is sent. NULL is accepted.
 */
void ost_proto_close(ost_proto *proto);

/*
 * Optional observer of the close order: called with OST_PROTO_CLOSE_STEP_DEVICE
 * for the device handle, then with the pipe index for each pipe handle.
 */
#define OST_PROTO_CLOSE_STEP_DEVICE 0xdeu
typedef void (*ost_proto_close_hook)(void *context, unsigned int step);
void ost_proto_set_close_hook(ost_proto *proto, ost_proto_close_hook hook, void *context);

#endif
