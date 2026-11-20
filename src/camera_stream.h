/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#ifndef OPENST133_CAMERA_STREAM_H
#define OPENST133_CAMERA_STREAM_H

#include "camera_acquire.h"
#include "openst133.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Streaming acquisition arms first-frame cycle once, keeps image IN
 * transfers outstanding and assembles frames from endpoint 0x82 until the
 * caller stops or requested frame count arrives. The stop tail follows in
 * both cases. The settings plan supplies the frame size of the selected
 * geometry.
 */

/*
 * Image transfers are a multiple of the 512-byte bulk packet. Enough transfers
 * stay outstanding to hold more than one frame (10 x 32768 = 327680 bytes).
 */
#define OST_STREAM_CHUNK_BYTES 32768u
#define OST_STREAM_DEPTH 10u

/*
 * Source of image data: a pipe of chunk-sized IN transfers kept outstanding on one endpoint. wait returns the next
 * completed transfer in submission order (data valid until the next wait or close; backlog gets the number of further transfers that had
 * already completed, so a backlog equal to the depth minus one means the host fell behind), OST_TRANSPORT_ERR_TIMEOUT
 * (OST_ERR_TIMEOUT for foreign sources) when nothing completed within timeout_ms, or an error status. A short
 * completion is data. close cancels everything still outstanding, waits until no transfer is in flight and reports
 * in discarded the bytes that completed or were cancelled without being consumed. libusb_transport.c provides the
 * libusb implementation; tests provide replay sources.
 */
/*
 * Register 0x3c is 0 for Focus, 1 for a single frame and 10 for a device-limited
 * ten-frame Acquire. Other finite counts run as Focus and the host stops after
 * the requested number of frames.
 */
#define OST_STREAM_DEVICE_FRAME_COUNT 10u

typedef struct ost_stream_source {
    void *userdata;
    int (*open)(void *userdata, ost_transport_handle *handle, unsigned char endpoint, size_t chunk_size,
        unsigned int depth, void **pipe);
    int (*wait)(void *userdata, void *pipe, unsigned int timeout_ms, const unsigned char **data, size_t *length,
        unsigned int *backlog);
    int (*close)(void *userdata, void *pipe, size_t *discarded_bytes);
} ost_stream_source;

const ost_stream_source *ost_libusb_stream_source(void);

typedef struct ost_stream_run_options {
    /* Chunk size and depth; zero selects OST_STREAM_CHUNK_BYTES and OST_STREAM_DEPTH. */
    size_t chunk_size;
    unsigned int depth;
    /* NULL selects the libusb source when the device is on the libusb transport, otherwise unsupported. */
    const ost_stream_source *source;
    /* Longest wait for the next chunk beyond the first frame, in milliseconds; zero selects 10000. */
    unsigned int idle_timeout_ms;
} ost_stream_run_options;

/*
 * Run a stream. frame_count 0 streams until options->abort_requested becomes nonzero; otherwise it stops after
 * that many frames. callback receives each frame in order and may keep it only for the duration of the call; a
 * non-success return stops the stream (the stop tail is still sent) and is returned, except OST_ERR_ABORTED which
 * ends the stream normally. A stop request (abort_requested) is a normal end: OST_SUCCESS with stats->stopped set.
 * stats is filled in on every return, including failures. Bytes of an incomplete trailing frame are counted in
 * stats->partial_bytes_discarded and never promoted to a frame.
 */
int ost_camera_stream_run(
    ost_device *device,
    const ost_acquire_options *options,
    const ost_stream_run_options *stream_options,
    uint32_t frame_count,
    ost_camera_frame_callback callback,
    void *userdata,
    ost_stream_stats *stats,
    char *error,
    size_t error_size);

#endif
