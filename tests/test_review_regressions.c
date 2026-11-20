/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

/* Regression tests for CLI diagnostics, error handling and data conversion. */

#define _POSIX_C_SOURCE 200809L

#include "openst133.h"
#include "openst133_spe.h"
#include "test_check.h"

#include <stdint.h>

/* The CLI is compiled in so that its static error labels can be checked without a camera. */
#define main ost_cli_main
#include "../src/cli/ost.c"
#undef main

int ost_temperature_main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return EXIT_FAILURE;
}

static char k_directory[1024];

static void put_u16(unsigned char *bytes, uint16_t value)
{
    bytes[0] = (unsigned char)(value & 0xffu);
    bytes[1] = (unsigned char)(value >> 8);
}

static void put_u32(unsigned char *bytes, uint32_t value)
{
    put_u16(bytes, (uint16_t)(value & 0xffffu));
    put_u16(bytes + 2, (uint16_t)(value >> 16));
}

static void path_in_directory(char *path, size_t size, const char *name)
{
    TEST_CHECK(snprintf(path, size, "%s/%s", k_directory, name) < (int)size);
}

/*
 * SPE 2.x header whose frame_count * xdim * ydim * 8 wraps 2^64 to the 5184 payload bytes present:
 * 2147090468 * (57170 * 18785 * 8) = 2^64 + 5184.
 */
static void test_spe2_size_overflow_is_rejected(void)
{
    static const uint32_t frame_count = 2147090468u;
    const size_t payload = 5184u;
    unsigned char *bytes = calloc(1u, OST_SPE_HEADER_SIZE + payload);
    float version = 2.5f;
    uint32_t version_bits;
    char path[1100];
    ost_spe_file *file = NULL;
    FILE *stream;
    int status;

    TEST_CHECK(bytes != NULL);
    memcpy(&version_bits, &version, sizeof(version_bits));
    put_u16(bytes + 42u, 57170u);
    put_u16(bytes + 108u, (uint16_t)OST_SPE_FLOAT64);
    put_u16(bytes + 656u, 18785u);
    put_u32(bytes + 1446u, frame_count);
    put_u32(bytes + 1992u, version_bits);
    path_in_directory(path, sizeof(path), "overflow.spe");
    stream = fopen(path, "wb");
    TEST_CHECK(stream != NULL);
    TEST_CHECK(fwrite(bytes, 1u, OST_SPE_HEADER_SIZE + payload, stream) == OST_SPE_HEADER_SIZE + payload);
    TEST_CHECK(fclose(stream) == 0);
    free(bytes);

    status = ost_spe_read(path, &file);
    if (status == OST_SPE_SUCCESS) {
        ost_spe_info info;

        TEST_CHECK(ost_spe_get_info(file, &info) == OST_SPE_SUCCESS);
        (void)fprintf(stderr, "accepted a %zu-byte SPE claiming %llu frames of %llu pixels\n",
            OST_SPE_HEADER_SIZE + payload, (unsigned long long)info.frame_count,
            (unsigned long long)info.pixel_count);
        ost_spe_free(file);
    }
    TEST_CHECK(status == OST_SPE_ERR_FORMAT);
    TEST_CHECK(file == NULL);
    TEST_CHECK(remove(path) == 0);
    (void)printf("SPE 2.x header with a wrapping frame size is rejected as malformed\n");
}

/*
 * The settings planner accepts a horizontal bin larger than the ROI width and reads one column (camera_scan.c
 * resolve_axis, ost.c output_width); saving that frame must succeed, as it already does for the vertical axis.
 */
static void test_spe_metadata_accepts_bin_wider_than_roi(void)
{
    uint16_t frames[100];
    ost_spe_metadata metadata;
    ost_spe_metadata read_back;
    ost_spe_file *file = NULL;
    ost_spe_info info;
    char path[1100];
    size_t i;

    for (i = 0u; i < sizeof(frames) / sizeof(frames[0]); ++i) {
        frames[i] = (uint16_t)i;
    }
    ost_spe_metadata_init(&metadata);
    metadata.exposure_seconds = 0.01;
    metadata.acquired_utc = 1767225600;
    metadata.roi_known = 1;
    metadata.roi_x = 0u;
    metadata.roi_y = 0u;
    metadata.roi_width = 10u;
    metadata.roi_height = 100u;
    metadata.x_binning = 20u;
    metadata.y_binning = 1u;
    path_in_directory(path, sizeof(path), "wide-bin.spe");
    TEST_CHECK(ost_spe_create_with_metadata(path, 1u, 100u, OST_SPE_UINT16, 1u, frames, sizeof(frames),
        &metadata) == OST_SPE_SUCCESS);
    TEST_CHECK(ost_spe_read(path, &file) == OST_SPE_SUCCESS);
    TEST_CHECK(ost_spe_get_info(file, &info) == OST_SPE_SUCCESS);
    TEST_CHECK(info.width == 1u && info.height == 100u && info.frame_count == 1u);
    TEST_CHECK(ost_spe_get_metadata(file, &read_back) == OST_SPE_SUCCESS);
    TEST_CHECK(read_back.roi_known && read_back.roi_width == 10u && read_back.x_binning == 20u);
    ost_spe_free(file);
    TEST_CHECK(remove(path) == 0);

    metadata.roi_width = 1340u;
    metadata.x_binning = 1u;
    metadata.roi_height = 40u;
    metadata.y_binning = 100u;
    path_in_directory(path, sizeof(path), "tall-bin.spe");
    TEST_CHECK(ost_spe_create_with_metadata(path, 1u, 100u, OST_SPE_UINT16, 1u, frames, sizeof(frames),
        &metadata) == OST_SPE_ERR_INVALID_ARGUMENT);
    (void)printf("SPE metadata with a horizontal bin wider than the ROI saves a one-column frame\n");
}

struct stub {
    unsigned int transfers;
};

static int stub_list(void *userdata, ost_device_info **devices, size_t *count)
{
    (void)userdata;
    *devices = NULL;
    *count = 0u;
    return OST_SUCCESS;
}

static void stub_free(void *userdata, ost_device_info *devices)
{
    (void)userdata;
    free(devices);
}

static int stub_open(void *userdata, const ost_device_info *info, ost_transport_handle **handle)
{
    (void)info;
    *handle = (ost_transport_handle *)userdata;
    return OST_SUCCESS;
}

static void stub_close(void *userdata, ost_transport_handle *handle)
{
    (void)userdata;
    (void)handle;
}

static int stub_control(void *userdata, ost_transport_handle *handle, uint8_t request_type, uint8_t request,
    uint16_t value, uint16_t index, unsigned char *data, uint16_t length, unsigned int timeout_ms, int *transferred)
{
    struct stub *stub = userdata;

    (void)handle;
    (void)request_type;
    (void)request;
    (void)value;
    (void)index;
    (void)data;
    (void)length;
    (void)timeout_ms;
    (void)transferred;
    ++stub->transfers;
    return OST_ERR_IO;
}

static int stub_bulk(void *userdata, ost_transport_handle *handle, unsigned char endpoint, unsigned char *data,
    int length, unsigned int timeout_ms, int *transferred)
{
    struct stub *stub = userdata;

    (void)handle;
    (void)endpoint;
    (void)data;
    (void)length;
    (void)timeout_ms;
    (void)transferred;
    ++stub->transfers;
    return OST_ERR_IO;
}

static void prime_unrelated_error(ost_camera *camera)
{
    TEST_CHECK(ost_camera_set_shutter(camera, OST_SHUTTER_CLOSED) == OST_ERR_UNSUPPORTED);
    TEST_CHECK(strstr(ost_camera_last_error(camera), "not supported yet") != NULL);
}

static void check_fresh_invalid_argument(ost_camera *camera, int status, const char *call)
{
    const char *text = ost_camera_last_error(camera);

    if (status != OST_ERR_INVALID_ARGUMENT || strstr(text, "not supported yet") != NULL ||
        strncmp(text, "invalid argument", 16u) != 0) {
        (void)fprintf(stderr, "%s returned %d with last error \"%s\"\n", call, status, text);
    }
    TEST_CHECK(status == OST_ERR_INVALID_ARGUMENT);
    TEST_CHECK(strstr(text, "not supported yet") == NULL);
    TEST_CHECK(strncmp(text, "invalid argument", 16u) == 0);
}

/* ost_camera_last_error documents "the most recent failure"; an invalid argument must not report an older one. */
static void test_invalid_argument_replaces_last_error(void)
{
    struct stub stub = {0u};
    ost_transport transport;
    ost_device_info info = {OST_USB_VENDOR_ID, OST_USB_PRODUCT_ID, 1u, 2u, 0u, {0u}};
    ost_camera *camera = NULL;
    ost_stream_stats stats;
    size_t acquired = 0u;
    unsigned char byte = 0u;

    memset(&transport, 0, sizeof(transport));
    transport.userdata = &stub;
    transport.list_devices = stub_list;
    transport.free_devices = stub_free;
    transport.open = stub_open;
    transport.close = stub_close;
    transport.control_transfer = stub_control;
    transport.bulk_transfer = stub_bulk;
    TEST_CHECK(ost_camera_open(&transport, &info, &camera) ==
        OST_SUCCESS);

    prime_unrelated_error(camera);
    check_fresh_invalid_argument(camera, ost_camera_set_exposure(camera, 0u), "set_exposure(0)");
    prime_unrelated_error(camera);
    check_fresh_invalid_argument(camera, ost_camera_set_shutter(camera, (ost_shutter_state)7), "set_shutter(7)");
    prime_unrelated_error(camera);
    check_fresh_invalid_argument(camera, ost_camera_set_readout(camera, NULL), "set_readout(NULL)");
    prime_unrelated_error(camera);
    check_fresh_invalid_argument(camera, ost_camera_set_readout_rate(camera, 0u), "set_readout_rate(0)");
    prime_unrelated_error(camera);
    check_fresh_invalid_argument(camera, ost_camera_acquire(camera, NULL, 1u, &acquired), "acquire(NULL)");
    prime_unrelated_error(camera);
    check_fresh_invalid_argument(camera, ost_camera_acquire(camera, &byte, 1u, NULL), "acquire(size NULL)");
    prime_unrelated_error(camera);
    check_fresh_invalid_argument(camera, ost_camera_stream(camera, 1u, NULL, NULL, &stats), "stream(NULL)");
    prime_unrelated_error(camera);
    check_fresh_invalid_argument(camera, ost_camera_get_temperature(camera, NULL), "get_temperature(NULL)");
    TEST_CHECK(stub.transfers == 0u);
    ost_camera_close(camera);
    (void)printf("invalid arguments replace the camera's last error instead of keeping an older one\n");
}

/* The CLI prints this label before every library status; timeouts and aborts must not read "unknown". */
static void test_cli_labels_every_library_status(void)
{
    static const int statuses[] = {
        OST_ERR_INVALID_ARGUMENT, OST_ERR_NO_MEMORY, OST_ERR_IO, OST_ERR_NOT_FOUND, OST_ERR_UNSUPPORTED,
        OST_ERR_ABORTED, OST_ERR_BUFFER_TOO_SMALL, OST_ERR_TIMEOUT, OST_ERR_BUSY, OST_ERR_ACCESS,
    };
    size_t i;

    for (i = 0u; i < sizeof(statuses) / sizeof(statuses[0]); ++i) {
        const char *label = status_text(statuses[i], 0);

        if (strstr(label, "unknown") != NULL) {
            (void)fprintf(stderr, "library status %d is labelled \"%s\"\n", statuses[i], label);
        }
        TEST_CHECK(strstr(label, "unknown") == NULL);
    }
    TEST_CHECK(strcmp(status_text(OST_ERR_TIMEOUT, 0), "timeout") == 0);
    TEST_CHECK(strcmp(status_text(OST_ERR_ABORTED, 0), "aborted") == 0);
    TEST_CHECK(strcmp(status_text(OST_SPE_ERR_FORMAT, 1), "malformed SPE file") == 0);
    TEST_CHECK(strcmp(status_text(OST_SPE_ERR_RANGE, 1), "frame index out of range") == 0);
    (void)printf("CLI labels every library status\n");
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        (void)fprintf(stderr, "usage: %s DATA_DIRECTORY\n", argv[0]);
        return EXIT_FAILURE;
    }
    TEST_CHECK(snprintf(k_directory, sizeof(k_directory), "%s/review-XXXXXX", argv[1]) < (int)sizeof(k_directory));
    TEST_CHECK(test_mkdtemp(k_directory) != NULL);
    test_spe2_size_overflow_is_rejected();
    test_spe_metadata_accepts_bin_wider_than_roi();
    test_invalid_argument_replaces_last_error();
    test_cli_labels_every_library_status();
#ifdef _WIN32
    TEST_CHECK(_rmdir(k_directory) == 0);
#else
    TEST_CHECK(rmdir(k_directory) == 0);
#endif
    return EXIT_SUCCESS;
}
