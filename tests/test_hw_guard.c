/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#include "hw_guard.h"
#include "openst133.h"
#include "pcap_usbmon.h"
#include "test_check.h"
#include "transport_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_bytes(unsigned char *output, uint32_t value, size_t width)
{
    size_t index;

    for (index = 0u; index < width; ++index) {
        output[index] = (unsigned char)((value >> (8u * index)) & 0xffu);
    }
}

static void write_bytes(FILE *file, const void *data, size_t size)
{
    TEST_CHECK(fwrite(data, 1u, size, file) == size);
}

static void write_number(FILE *file, uint32_t value, size_t width)
{
    unsigned char bytes[4] = {0};

    put_bytes(bytes, value, width);
    write_bytes(file, bytes, width);
}

static void write_replay_capture(const char *path)
{
    static const unsigned char descriptor[] = {
        0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40,
        0xd7, 0x0b, 0x10, 0xa0, 0x00, 0x01, 0x01, 0x02, 0x03, 0x01,
    };
    FILE *file = fopen(path, "wb");
    unsigned int frame;

    TEST_CHECK(file != NULL);
    write_number(file, 0xa1b2c3d4u, 4u);
    write_number(file, 2u, 2u);
    write_number(file, 4u, 2u);
    write_number(file, 0u, 4u);
    write_number(file, 0u, 4u);
    write_number(file, 65535u, 4u);
    write_number(file, 189u, 4u);
    for (frame = 0u; frame < 2u; ++frame) {
        unsigned char header[48] = {0};
        size_t payload = frame == 1u ? sizeof(descriptor) : 0u;

        write_number(file, 100u, 4u);
        write_number(file, frame + 1u, 4u);
        write_number(file, 48u + (uint32_t)payload, 4u);
        write_number(file, 48u + (uint32_t)payload, 4u);
        put_bytes(header, 0x01020304u, 4u);
        put_bytes(header + 4u, 0x05060700u, 4u);
        header[8] = frame == 0u ? 'S' : 'C';
        header[9] = 2u;
        header[10] = 0x80u;
        header[11] = 7u;
        put_bytes(header + 12u, 2u, 2u);
        header[14] = frame == 0u ? 0u : 0xffu;
        header[15] = payload == 0u ? 0xffu : 0u;
        put_bytes(header + 16u, 100u, 4u);
        put_bytes(header + 24u, frame + 1u, 4u);
        put_bytes(header + 32u, frame == 0u ? 64u : (uint32_t)sizeof(descriptor), 4u);
        put_bytes(header + 36u, (uint32_t)payload, 4u);
        header[40] = 0x80u;
        header[41] = 6u;
        put_bytes(header + 42u, 0x0100u, 2u);
        put_bytes(header + 46u, 64u, 2u);
        write_bytes(file, header, sizeof(header));
        if (payload != 0u) {
            write_bytes(file, descriptor, payload);
        }
    }
    TEST_CHECK(fclose(file) == 0);
}

static void test_guard_values(void)
{
    const ost_device_info info = {OST_USB_VENDOR_ID, OST_USB_PRODUCT_ID, 255u, 255u, 0u, {0u}};
    ost_transport_handle *handle = NULL;
    ost_device *device = NULL;
    ost_camera *camera = NULL;

    TEST_CHECK(test_unsetenv(OST_NO_HARDWARE_ENV) == 0);
    TEST_CHECK(ost_hw_guard_check() == OST_SUCCESS);
    TEST_CHECK(test_setenv(OST_NO_HARDWARE_ENV, "0", 1) == 0);
    TEST_CHECK(ost_hw_guard_check() == OST_SUCCESS);
    TEST_CHECK(test_setenv(OST_NO_HARDWARE_ENV, "true", 1) == 0);
    TEST_CHECK(ost_hw_guard_check() == OST_SUCCESS);
    TEST_CHECK(test_setenv(OST_NO_HARDWARE_ENV, "1", 1) == 0);
    TEST_CHECK(ost_hw_guard_check() == OST_ERR_UNSUPPORTED);
    TEST_CHECK(strstr(ost_hw_guard_error(), OST_NO_HARDWARE_ENV) != NULL);
    TEST_CHECK(ost_libusb_transport()->open(NULL, &info, &handle) == OST_ERR_UNSUPPORTED);
    TEST_CHECK(handle == NULL);
    TEST_CHECK(ost_open(NULL, &info, &device) == OST_ERR_UNSUPPORTED);
    TEST_CHECK(device == NULL);
    TEST_CHECK(ost_camera_open(NULL, &info, &camera) == OST_ERR_UNSUPPORTED);
    TEST_CHECK(camera == NULL);
    TEST_CHECK(strstr(ost_camera_open_error(), OST_NO_HARDWARE_ENV) != NULL);
}

static void test_replay_under_guard(const char *directory)
{
    char path[1024];
    char error[256];
    ost_replay_transport *replay = NULL;
    const ost_transport *transport;
    ost_device_info *devices = NULL;
    size_t count = 0u;
    ost_device *device = NULL;

    TEST_CHECK(snprintf(path, sizeof(path), "%s/hw-guard-replay.pcap", directory) > 0);
    write_replay_capture(path);
    TEST_CHECK(test_setenv(OST_NO_HARDWARE_ENV, "1", 1) == 0);
    TEST_CHECK(ost_replay_transport_create(path, &replay, error, sizeof(error)) == OST_SUCCESS);
    transport = ost_replay_transport_interface(replay);
    TEST_CHECK(ost_list_devices(transport, &devices, &count) == OST_SUCCESS);
    TEST_CHECK(count == 1u);
    TEST_CHECK(ost_open(transport, &devices[0], &device) == OST_SUCCESS);
    ost_close(device);
    ost_free_devices(devices);
    ost_replay_transport_destroy(replay);
}

int main(int argc, char **argv)
{
    TEST_CHECK(argc == 2);
    test_guard_values();
    test_replay_under_guard(argv[1]);
    TEST_CHECK(test_unsetenv(OST_NO_HARDWARE_ENV) == 0);
    puts("hardware guard refuses real opens and permits replay");
    return 0;
}
