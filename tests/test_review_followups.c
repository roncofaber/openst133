/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

/* Regression tests for stream interruption. */

#define _POSIX_C_SOURCE 200809L

#include "openst133.h"
#include "test_check.h"

#include <signal.h>
#include <stdint.h>

/* The CLI is compiled in so that its static stream interrupt handler can be exercised without a camera. */
#define main ost_cli_main
#include "../src/cli/ost.c"
#undef main

int ost_temperature_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return EXIT_FAILURE;
}

/*
 * Claiming the camera clears its abort request, so a Ctrl-C whose ost_camera_abort lands before
 * ost_camera_stream claims the camera was lost and an unbounded stream kept running. The interrupt must also stop the
 * stream from the frame callback, after keeping the frame that arrived.
 */
static void test_stream_interrupt_survives_the_claim(void)
{
    unsigned char kept[4] = {0u, 0u, 0u, 0u};
    const unsigned char arrived[4] = {1u, 2u, 3u, 4u};
    acquire_collection collection;
    int status;

    collection.frames = kept;
    collection.frame_size = sizeof(kept);
    collection.frame_count = 1u;
    collection.keep_last = 1;
    stream_camera = NULL;
    stream_interrupt(SIGINT);
    status = collect_acquired_frame(&collection, 0u, arrived, sizeof(arrived));
    if (status != OST_ERR_ABORTED) {
        (void)fprintf(stderr, "frame callback after Ctrl-C returned %d, expected OST_ERR_ABORTED\n", status);
    }
    TEST_CHECK(status == OST_ERR_ABORTED);
    TEST_CHECK(memcmp(kept, arrived, sizeof(kept)) == 0);
    (void)printf("stream interrupt survives the camera claim\n");
}

int main(void)
{
    test_stream_interrupt_survives_the_claim();
    return EXIT_SUCCESS;
}
