/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#include "camera_stream.h"

#include "protocol.h"
#include "transport_internal.h"
#include "time_util.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_ENDPOINT 0x82u
#define COMMAND_ENDPOINT 0x08u
#define WAIT_SLICE_MS 50u
#define DEFAULT_IDLE_TIMEOUT_MS 10000u
#define ARM_COMMAND 0x0014u
#define ARM_ARGUMENT 0x0100u

typedef struct engine {
    ost_device *device;
    ost_transport saved;
    ost_transport_handle *saved_handle;
    const ost_stream_source *source;
    void *pipe;
    size_t chunk_size;
    unsigned int depth;
    unsigned int idle_timeout_ms;
    const unsigned char *current;
    size_t current_left;
    unsigned char *frame;
    size_t frame_bytes;
    size_t served;
    uint32_t frame_count;
    ost_camera_frame_callback callback;
    void *userdata;
    atomic_int *abort_requested;
    ost_stream_stats *stats;
    unsigned char arm_record[OST_PROTO_SINGLE_COMMAND_SIZE];
    int first_frame_done;
    int status;
    char *error;
    size_t error_size;
    double last_frame_time;
    double last_period;
} engine;

static void fail(engine *e, const char *format, ...)
{
    va_list arguments;

    if (e->error == NULL || e->error_size == 0u) {
        return;
    }
    va_start(arguments, format);
    (void)vsnprintf(e->error, e->error_size, format, arguments);
    va_end(arguments);
}

static double now_seconds(void)
{
    return ost_time_seconds();
}

static int is_timeout(int status)
{
    return status == OST_TRANSPORT_ERR_TIMEOUT || status == OST_ERR_TIMEOUT;
}

static int open_pipe(engine *e)
{
    int status;

    if (e->pipe != NULL) {
        return OST_SUCCESS;
    }
    status = e->source->open(e->source->userdata, e->saved_handle, IMAGE_ENDPOINT, e->chunk_size, e->depth, &e->pipe);
    if (status != OST_SUCCESS || e->pipe == NULL) {
        fail(e, "cannot start the image pipe (%s)", ost_status_text(status != OST_SUCCESS ? status : OST_ERR_IO));
        e->pipe = NULL;
        return status != OST_SUCCESS ? status : OST_ERR_IO;
    }
    return OST_SUCCESS;
}

/*
 * Copy length bytes of the image byte stream to dst. Chunk boundaries and frame boundaries are unrelated: a
 * request may span chunks and a chunk may span requests. With abortable set a stop request ends the wait with
 * OST_ERR_ABORTED; the first frame is not abortable here because the acquisition sequence supplies its own stop
 * exchange between reads.
 */
static int pull(engine *e, unsigned char *dst, size_t length, unsigned int first_timeout_ms, int abortable,
    size_t *copied)
{
    unsigned int budget = first_timeout_ms;
    size_t done = 0u;

    *copied = 0u;

    while (done < length) {
        size_t take;

        if (e->current_left == 0u) {
            const unsigned char *data = NULL;
            size_t size = 0u;
            unsigned int backlog = 0u;
            unsigned int waited = 0u;
            int status;

            for (;;) {
                unsigned int slice = budget - waited < WAIT_SLICE_MS ? budget - waited : WAIT_SLICE_MS;

                if (abortable && e->abort_requested != NULL && atomic_load(e->abort_requested) != 0) {
                    return OST_ERR_ABORTED;
                }
                status = e->source->wait(e->source->userdata, e->pipe, slice, &data, &size, &backlog);
                if (status == OST_SUCCESS) {
                    break;
                }
                if (!is_timeout(status)) {
                    fail(e, "image transfer failed (%s) after %llu bytes", ost_status_text(status),
                        (unsigned long long)e->stats->bytes_received);
                    return status;
                }
                waited += slice;
                if (waited >= budget) {
                    fail(e, "no image data for %u ms after %llu bytes", budget,
                        (unsigned long long)e->stats->bytes_received);
                    return OST_ERR_TIMEOUT;
                }
            }
            if (size == 0u) {
                continue;
            }
            e->current = data;
            e->current_left = size;
            e->stats->bytes_received += size;
            ++e->stats->transfers_completed;
            if (size < e->chunk_size) {
                ++e->stats->short_transfers;
            }
            if (e->depth > 1u && backlog + 1u >= e->depth) {
                ++e->stats->host_stalls;
            }
            if (backlog + 1u > e->stats->max_ready) {
                e->stats->max_ready = backlog + 1u;
            }
            budget = e->idle_timeout_ms;
        }
        take = length - done < e->current_left ? length - done : e->current_left;
        memcpy(dst + done, e->current, take);
        e->current += take;
        e->current_left -= take;
        done += take;
        *copied = done;
    }
    return OST_SUCCESS;
}

static void deliver(engine *e)
{
    double start = now_seconds();
    double arrived = start;
    double period = e->last_frame_time > 0.0 ? arrived - e->last_frame_time : 0.0;
    double spent;
    uint32_t index = e->stats->frames_delivered;
    int status;

    ++e->stats->frames_delivered;
    status = e->callback(e->userdata, index, e->frame, e->frame_bytes);
    spent = now_seconds() - start;
    e->last_frame_time = arrived;
    if (spent * 1e6 > (double)e->stats->max_callback_us) {
        e->stats->max_callback_us = (uint32_t)(spent * 1e6);
    }
    if (e->last_period > 0.0 && spent > e->last_period) {
        ++e->stats->frames_lagged;
    }
    if (period > 0.0) {
        e->last_period = period;
    }
    if (status == OST_ERR_ABORTED) {
        e->stats->stopped = 1;
    } else if (status != OST_SUCCESS) {
        e->status = status;
        fail(e, "frame callback stopped the stream after %u complete frame%s", (unsigned int)e->stats->frames_delivered,
            e->stats->frames_delivered == 1u ? "" : "s");
    }
}

static int keep_going(const engine *e)
{
    if (e->status != OST_SUCCESS || e->stats->stopped) {
        return 0;
    }
    if (e->frame_count != 0u && e->stats->frames_delivered >= e->frame_count) {
        return 0;
    }
    if (e->abort_requested != NULL && atomic_load(e->abort_requested) != 0) {
        return 0;
    }
    return 1;
}

static void stream_frames(engine *e)
{
    deliver(e);
    while (keep_going(e)) {
        size_t fill = 0u;
        int status = pull(e, e->frame, e->frame_bytes, e->idle_timeout_ms, 1, &fill);

        if (status == OST_ERR_ABORTED) {
            e->stats->stopped = 1;
            e->stats->partial_bytes_discarded += fill;
            return;
        }
        if (status != OST_SUCCESS) {
            e->status = status;
            e->stats->partial_bytes_discarded += fill;
            return;
        }
        deliver(e);
    }
}

static int proxy_control(void *userdata, ost_transport_handle *handle, uint8_t request_type, uint8_t request,
    uint16_t value, uint16_t index, unsigned char *data, uint16_t length, unsigned int timeout_ms, int *transferred)
{
    engine *e = userdata;

    (void)handle;
    return e->saved.control_transfer(e->saved.userdata, e->saved_handle, request_type, request, value, index, data,
        length, timeout_ms, transferred);
}

static int proxy_bulk(void *userdata, ost_transport_handle *handle, unsigned char endpoint, unsigned char *data,
    int length, unsigned int timeout_ms, int *transferred)
{
    engine *e = userdata;
    size_t copied = 0u;
    int status;

    (void)handle;
    if (endpoint == COMMAND_ENDPOINT && length == (int)sizeof(e->arm_record) && e->pipe == NULL &&
        !e->first_frame_done && memcmp(data, e->arm_record, sizeof(e->arm_record)) == 0) {
        status = open_pipe(e);
        if (status != OST_SUCCESS) {
            return status;
        }
    }
    if (endpoint != IMAGE_ENDPOINT) {
        return e->saved.bulk_transfer(e->saved.userdata, e->saved_handle, endpoint, data, length, timeout_ms,
            transferred);
    }
    if (length < 0 || data == NULL || transferred == NULL || e->first_frame_done) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    status = open_pipe(e);
    if (status != OST_SUCCESS) {
        return status;
    }
    status = pull(e, data, (size_t)length, timeout_ms, 0, &copied);
    if (status != OST_SUCCESS) {
        e->status = status;
        return status;
    }
    *transferred = length;
    e->served += (size_t)length;
    if (e->served >= e->frame_bytes) {
        e->first_frame_done = 1;
        stream_frames(e);
    }
    return OST_SUCCESS;
}

int ost_camera_stream_run(
    ost_device *device,
    const ost_acquire_options *options,
    const ost_stream_run_options *stream_options,
    uint32_t frame_count,
    ost_camera_frame_callback callback,
    void *userdata,
    ost_stream_stats *stats,
    char *error,
    size_t error_size)
{
    engine e;
    ost_transport proxy;
    ost_transport_handle *handle = NULL;
    ost_acquire_options run_options;
    const ost_stream_source *source = NULL;
    size_t discarded = 0u;
    size_t received = 0u;
    int rc;
    int status;

    if (stats != NULL) {
        memset(stats, 0, sizeof(*stats));
    }
    if (device == NULL || options == NULL || options->plan == NULL || callback == NULL || stats == NULL ||
        (frame_count == 0u && options->abort_requested == NULL)) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    if (error != NULL && error_size != 0u) {
        error[0] = '\0';
    }
    if (options->plan->frame_bytes == 0u) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    if (options->plan->accumulations > 1u) {
        if (error != NULL && error_size != 0u) {
            (void)snprintf(error, error_size, "a stream programs register 0x3c itself and cannot accumulate");
        }
        return OST_ERR_UNSUPPORTED;
    }
    memset(&e, 0, sizeof(e));
    e.device = device;
    e.chunk_size = stream_options != NULL && stream_options->chunk_size != 0u ? stream_options->chunk_size
                                                                               : OST_STREAM_CHUNK_BYTES;
    e.depth = stream_options != NULL && stream_options->depth != 0u ? stream_options->depth : OST_STREAM_DEPTH;
    e.idle_timeout_ms = stream_options != NULL && stream_options->idle_timeout_ms != 0u
        ? stream_options->idle_timeout_ms : DEFAULT_IDLE_TIMEOUT_MS;
    e.frame_bytes = options->plan->frame_bytes;
    e.frame_count = frame_count;
    e.callback = callback;
    e.userdata = userdata;
    e.abort_requested = options->abort_requested;
    e.stats = stats;
    e.error = error;
    e.error_size = error_size;
    source = stream_options != NULL ? stream_options->source : NULL;
    if (ost_proto_encode_command(e.arm_record, sizeof(e.arm_record), ARM_COMMAND, ARM_ARGUMENT) !=
        sizeof(e.arm_record)) {
        return OST_ERR_IO;
    }
    e.frame = malloc(e.frame_bytes);
    if (e.frame == NULL) {
        fail(&e, "out of memory for one stream frame");
        return OST_ERR_NO_MEMORY;
    }
    memset(&proxy, 0, sizeof(proxy));
    proxy.userdata = &e;
    proxy.control_transfer = proxy_control;
    proxy.bulk_transfer = proxy_bulk;
    status = ost_device_swap_transport(device, &proxy, &handle);
    if (status != OST_SUCCESS) {
        free(e.frame);
        return status;
    }
    e.saved = proxy;
    e.saved_handle = handle;
    if (source == NULL && e.saved.open == ost_libusb_transport()->open) {
        source = ost_libusb_stream_source();
    }
    if (source == NULL || source->open == NULL || source->wait == NULL || source->close == NULL ||
        e.saved.control_transfer == NULL || e.saved.bulk_transfer == NULL) {
        (void)ost_device_swap_transport(device, &e.saved, &e.saved_handle);
        fail(&e, "this transport has no asynchronous image pipe; streaming needs the libusb transport");
        free(e.frame);
        return OST_ERR_UNSUPPORTED;
    }
    e.source = source;
    run_options = *options;
    run_options.frame_count_set = 1;
    run_options.frame_count = frame_count == OST_STREAM_DEVICE_FRAME_COUNT ? (uint16_t)frame_count : 0u;
    rc = ost_acquire_run(device, &run_options, e.frame, e.frame_bytes, &received, error, error_size);
    {
        ost_transport restore = e.saved;
        ost_transport_handle *restore_handle = e.saved_handle;

        (void)ost_device_swap_transport(device, &restore, &restore_handle);
    }
    if (e.pipe != NULL) {
        int close_status = e.source->close(e.source->userdata, e.pipe, &discarded);

        e.pipe = NULL;
        if (close_status != OST_SUCCESS && e.status == OST_SUCCESS) {
            e.status = close_status;
            if (e.source == ost_libusb_stream_source()) {
                fail(&e, "USB image-pipe close timed out; the unresponsive device was closed (%s)",
                    ost_status_text(close_status));
            } else {
                fail(&e, "closing the image pipe failed (%s)", ost_status_text(close_status));
            }
        }
    }
    if (!e.first_frame_done) {
        stats->partial_bytes_discarded += e.served;
    }
    stats->partial_bytes_discarded += discarded + e.current_left;
    free(e.frame);
    if (e.status != OST_SUCCESS) {
        return e.status;
    }
    if (rc == OST_ERR_ABORTED) {
        stats->stopped = 1;
        return OST_SUCCESS;
    }
    return rc;
}
