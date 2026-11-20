/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#include "openst133.h"
#include "pcap_usbmon.h"
#include "test_check.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINKTYPE_USB_LINUX 189u
#define LINKTYPE_USB_LINUX_MMAPPED 220u

struct trace_event {
    uint8_t event;
    uint8_t transfer_type;
    uint8_t endpoint;
    uint16_t setup_value;
    uint16_t setup_index;
    uint16_t setup_length;
    uint8_t request_type;
    uint8_t request;
    int32_t status;
    uint32_t length;
    const unsigned char *data;
    uint32_t data_size;
};

static const unsigned char device_descriptor[] = {
    0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40,
    0xd7, 0x0b, 0x10, 0xa0, 0x00, 0x01, 0x01, 0x02, 0x03, 0x01,
};
static const unsigned char bulk_out_data[] = {1u, 2u, 3u, 4u};
static const unsigned char bulk_in_data[] = {9u, 8u, 7u};
static const unsigned char control_out_data[] = {0xaau, 0xbbu, 0xccu};
static const unsigned char control_in_data[] = {0xdeu, 0xadu, 0xbeu, 0xefu};

static const struct trace_event events_48[] = {
    {'S', 2u, 0x80u, 0x0100u, 0u, 64u, 0x80u, 6u, 0, 64u, NULL, 0u},
    {'C', 2u, 0x80u, 0x0100u, 0u, 64u, 0x80u, 6u, 0, 18u, device_descriptor, 18u},
    {'S', 3u, 0x02u, 0u, 0u, 0u, 0x02u, 0u, 0, 4u, bulk_out_data, 4u},
    {'C', 3u, 0x02u, 0u, 0u, 0u, 0x02u, 0u, 0, 4u, NULL, 0u},
    {'S', 3u, 0x82u, 0u, 0u, 0u, 0x82u, 0u, 0, 5u, NULL, 0u},
    {'C', 3u, 0x82u, 0u, 0u, 0u, 0x82u, 0u, 0, 3u, bulk_in_data, 3u},
};

static const struct trace_event events_64[] = {
    {'S', 2u, 0x80u, 0x0100u, 0u, 64u, 0x80u, 6u, 0, 64u, NULL, 0u},
    {'C', 2u, 0x80u, 0x0100u, 0u, 64u, 0x80u, 6u, 0, 18u, device_descriptor, 18u},
    {'S', 2u, 0x00u, 0x2233u, 0x4455u, 3u, 0x40u, 0x11u, 0, 3u, control_out_data, 3u},
    {'C', 2u, 0x00u, 0x2233u, 0x4455u, 3u, 0x40u, 0x11u, 0, 3u, NULL, 0u},
    {'S', 2u, 0x80u, 0u, 0u, 4u, 0xc0u, 0x12u, 0, 4u, NULL, 0u},
    {'C', 2u, 0x80u, 0u, 0u, 4u, 0xc0u, 0x12u, 0, 4u, control_in_data, 4u},
    {'S', 2u, 0x00u, 0u, 0u, 0u, 0x40u, 0x13u, 0, 0u, NULL, 0u},
    {'C', 2u, 0x00u, 0u, 0u, 0u, 0x40u, 0x13u, -5, 0u, NULL, 0u},
};

static void put_bytes(unsigned char *out, uint64_t value, size_t width, int big_endian)
{
    size_t i;

    for (i = 0u; i < width; ++i) {
        size_t shift = big_endian ? (width - i - 1u) * 8u : i * 8u;

        out[i] = (unsigned char)((value >> shift) & 0xffu);
    }
}

static void write_bytes(FILE *file, const unsigned char *bytes, size_t size)
{
    TEST_CHECK(fwrite(bytes, 1u, size, file) == size);
}

static void write_number(FILE *file, uint64_t value, size_t width, int big_endian)
{
    unsigned char bytes[8];

    put_bytes(bytes, value, width, big_endian);
    write_bytes(file, bytes, width);
}

static void write_trace(const char *path, const struct trace_event *events, size_t count, int big_endian)
{
    size_t usbmon_size = big_endian ? 64u : 48u;
    FILE *file = fopen(path, "wb");
    size_t i;

    TEST_CHECK(file != NULL);
    write_number(file, 0xa1b2c3d4u, 4u, big_endian);
    write_number(file, 2u, 2u, big_endian);
    write_number(file, 4u, 2u, big_endian);
    write_number(file, 0u, 4u, big_endian);
    write_number(file, 0u, 4u, big_endian);
    write_number(file, 65535u, 4u, big_endian);
    write_number(file, big_endian ? LINKTYPE_USB_LINUX_MMAPPED : LINKTYPE_USB_LINUX, 4u, big_endian);
    for (i = 0u; i < count; ++i) {
        const struct trace_event *event = &events[i];
        uint32_t captured = event->data_size;
        uint64_t id = 0x0102030405060700ULL + i / 2u;
        uint32_t packet_size = (uint32_t)usbmon_size + captured;
        unsigned char header[64] = {0};

        write_number(file, 100u, 4u, big_endian);
        write_number(file, i + 1u, 4u, big_endian);
        write_number(file, packet_size, 4u, big_endian);
        write_number(file, packet_size, 4u, big_endian);
        put_bytes(header + 0u, id, 8u, big_endian);
        header[8] = event->event;
        header[9] = event->transfer_type;
        header[10] = event->endpoint;
        header[11] = 7u;
        put_bytes(header + 12u, 2u, 2u, big_endian);
        header[14] = event->event == 'S' && event->transfer_type == 2u ? 0u : 0xffu;
        header[15] = captured == 0u ? 0xffu : 0u;
        put_bytes(header + 16u, 100u, 8u, big_endian);
        put_bytes(header + 24u, i + 1u, 4u, big_endian);
        put_bytes(header + 28u, (uint32_t)event->status, 4u, big_endian);
        put_bytes(header + 32u, event->length, 4u, big_endian);
        put_bytes(header + 36u, captured, 4u, big_endian);
        header[40] = event->request_type;
        header[41] = event->request;
        put_bytes(header + 42u, event->setup_value, 2u, 0);
        put_bytes(header + 44u, event->setup_index, 2u, 0);
        put_bytes(header + 46u, event->setup_length, 2u, 0);
        if (big_endian) {
            put_bytes(header + 48u, 10u, 4u, 1);
            put_bytes(header + 52u, 20u, 4u, 1);
            put_bytes(header + 56u, 30u, 4u, 1);
            put_bytes(header + 60u, 0u, 4u, 1);
        }
        write_bytes(file, header, usbmon_size);
        if (captured != 0u) {
            write_bytes(file, event->data, captured);
        }
    }
    TEST_CHECK(fclose(file) == 0);
}

static void make_path(char *buffer, size_t size, const char *directory, const char *name)
{
    TEST_CHECK(snprintf(buffer, size, "%s/%s", directory, name) > 0);
}

static void test_parser(const char *directory)
{
    char path[1024];
    ost_pcap_usbmon *capture = NULL;
    char error[256];

    make_path(path, sizeof(path), directory, "usbmon-48-le.pcap");
    write_trace(path, events_48, sizeof(events_48) / sizeof(events_48[0]), 0);
    TEST_CHECK(ost_pcap_usbmon_load(path, &capture, error, sizeof(error)) == OST_SUCCESS);
    TEST_CHECK(capture->count == 6u);
    TEST_CHECK(capture->records[0].event == OST_USBMON_SUBMISSION);
    TEST_CHECK(capture->records[1].event == OST_USBMON_COMPLETION);
    TEST_CHECK(capture->records[0].frame_number == 1u);
    TEST_CHECK(capture->records[0].id == 0x0102030405060700ULL);
    TEST_CHECK(capture->records[0].transfer_type == 2u);
    TEST_CHECK(capture->records[0].setup_present);
    TEST_CHECK(capture->records[0].setup[0] == 0x80u);
    TEST_CHECK(capture->records[0].setup[1] == 6u);
    TEST_CHECK(capture->records[0].bus_number == 2u);
    TEST_CHECK(capture->records[0].device_address == 7u);
    TEST_CHECK(capture->records[1].data_size == sizeof(device_descriptor));
    TEST_CHECK(memcmp(capture->records[1].data, device_descriptor, sizeof(device_descriptor)) == 0);
    TEST_CHECK(capture->records[2].length == 4u);
    TEST_CHECK(capture->records[2].data_size == 4u);
    TEST_CHECK(capture->records[5].length == 3u);
    TEST_CHECK(capture->records[5].captured_length == 3u);
    ost_pcap_usbmon_free(capture);

    make_path(path, sizeof(path), directory, "usbmon-64-be.pcap");
    write_trace(path, events_64, sizeof(events_64) / sizeof(events_64[0]), 1);
    capture = NULL;
    TEST_CHECK(ost_pcap_usbmon_load(path, &capture, error, sizeof(error)) == OST_SUCCESS);
    TEST_CHECK(capture->count == 8u);
    TEST_CHECK(capture->records[2].frame_number == 3u);
    TEST_CHECK(capture->records[2].setup[2] == 0x33u);
    TEST_CHECK(capture->records[2].setup[3] == 0x22u);
    TEST_CHECK(capture->records[2].setup[4] == 0x55u);
    TEST_CHECK(capture->records[2].setup[5] == 0x44u);
    TEST_CHECK(capture->records[3].status == 0);
    TEST_CHECK(capture->records[7].status == -5);
    TEST_CHECK(capture->records[5].data_size == 4u);
    TEST_CHECK(memcmp(capture->records[5].data, control_in_data, sizeof(control_in_data)) == 0);
    ost_pcap_usbmon_free(capture);
}

static void open_replay_device(const char *path, ost_replay_transport **replay, ost_device **device)
{
    const ost_transport *transport;
    ost_device_info *devices = NULL;
    size_t count = 0u;
    char error[256];

    TEST_CHECK(ost_replay_transport_create(path, replay, error, sizeof(error)) == OST_SUCCESS);
    transport = ost_replay_transport_interface(*replay);
    TEST_CHECK(transport != NULL);
    TEST_CHECK(ost_list_devices(transport, &devices, &count) == OST_SUCCESS);
    if (count != 1u) {
        (void)fprintf(stderr, "replay discovery returned %lu devices\n", (unsigned long)count);
    }
    TEST_CHECK(count == 1u);
    TEST_CHECK(devices[0].vendor_id == OST_USB_VENDOR_ID);
    TEST_CHECK(devices[0].product_id == OST_USB_PRODUCT_ID);
    TEST_CHECK(devices[0].bus_number == 2u);
    TEST_CHECK(devices[0].device_address == 7u);
    TEST_CHECK(ost_open(transport, &devices[0], device) == OST_SUCCESS);
    ost_free_devices(devices);
}

static void test_bulk_replay(const char *directory)
{
    char path[1024];
    ost_replay_transport *replay = NULL;
    ost_device *device = NULL;
    unsigned char output[4] = {0};
    unsigned char input[5] = {0};
    int transferred = -1;

    make_path(path, sizeof(path), directory, "usbmon-48-le.pcap");
    open_replay_device(path, &replay, &device);
    memcpy(output, bulk_out_data, sizeof(output));
    TEST_CHECK(ost_bulk_transfer(device, 0x02u, output, 4, 100u, &transferred) == OST_SUCCESS);
    TEST_CHECK(transferred == 4);
    TEST_CHECK(ost_bulk_transfer(device, 0x82u, input, 5, 100u, &transferred) == OST_SUCCESS);
    TEST_CHECK(transferred == 3);
    TEST_CHECK(memcmp(input, bulk_in_data, sizeof(bulk_in_data)) == 0);
    TEST_CHECK(ost_bulk_transfer(device, 0x02u, output, 4, 100u, &transferred) == OST_ERR_IO);
    TEST_CHECK(strstr(ost_replay_transport_last_error(replay), "end of capture") != NULL);
    ost_close(device);
    ost_replay_transport_destroy(replay);
}

static void test_control_replay(const char *directory)
{
    char path[1024];
    ost_replay_transport *replay = NULL;
    ost_device *device = NULL;
    unsigned char output[3] = {0};
    unsigned char input[4] = {0};
    int transferred = -1;

    make_path(path, sizeof(path), directory, "usbmon-64-be.pcap");
    open_replay_device(path, &replay, &device);
    memcpy(output, control_out_data, sizeof(control_out_data));
    TEST_CHECK(ost_control_transfer(device, 0x40u, 0x11u, 0x2233u, 0x4455u, output, 3u, 0u, &transferred) == OST_SUCCESS);
    TEST_CHECK(transferred == 3);
    TEST_CHECK(ost_control_transfer(device, 0xc0u, 0x12u, 0u, 0u, input, 4u, 0u, &transferred) == OST_SUCCESS);
    TEST_CHECK(transferred == 4);
    TEST_CHECK(memcmp(input, control_in_data, sizeof(control_in_data)) == 0);
    TEST_CHECK(ost_control_transfer(device, 0x40u, 0x13u, 0u, 0u, NULL, 0u, 0u, &transferred) == OST_ERR_IO);
    TEST_CHECK(strstr(ost_replay_transport_last_error(replay), "frame 7") != NULL);
    TEST_CHECK(strstr(ost_replay_transport_last_error(replay), "status -5") != NULL);
    ost_close(device);
    ost_replay_transport_destroy(replay);
}

static void test_mismatch(const char *directory)
{
    char path[1024];
    ost_replay_transport *replay = NULL;
    ost_device *device = NULL;
    unsigned char output[4] = {0};
    unsigned char input[4] = {0};
    int transferred = -1;

    make_path(path, sizeof(path), directory, "usbmon-64-be.pcap");
    open_replay_device(path, &replay, &device);
    TEST_CHECK(ost_control_transfer(device, 0x40u, 0x99u, 0x2233u, 0x4455u, output, 3u, 0u, &transferred) == OST_ERR_IO);
    TEST_CHECK(strstr(ost_replay_transport_last_error(replay), "frame 3") != NULL);
    memcpy(output, control_out_data, sizeof(control_out_data));
    TEST_CHECK(ost_control_transfer(device, 0x40u, 0x11u, 0x2233u, 0x4455u, output, 3u, 0u, &transferred) == OST_SUCCESS);
    TEST_CHECK(ost_bulk_transfer(device, 0x82u, input, 4, 0u, &transferred) == OST_ERR_IO);
    TEST_CHECK(strstr(ost_replay_transport_last_error(replay), "frame 5") != NULL);
    TEST_CHECK(ost_control_transfer(device, 0xc0u, 0x12u, 0u, 0u, input, 4u, 0u, &transferred) == OST_SUCCESS);
    ost_close(device);
    ost_replay_transport_destroy(replay);

    replay = NULL;
    device = NULL;
    make_path(path, sizeof(path), directory, "usbmon-48-le.pcap");
    open_replay_device(path, &replay, &device);
    memcpy(output, bulk_out_data, sizeof(output));
    output[1] = 0xffu;
    TEST_CHECK(ost_bulk_transfer(device, 0x02u, output, 4, 0u, &transferred) == OST_ERR_IO);
    TEST_CHECK(strstr(ost_replay_transport_last_error(replay), "frame 3") != NULL);
    TEST_CHECK(strstr(ost_replay_transport_last_error(replay), "data") != NULL);
    ost_close(device);
    ost_replay_transport_destroy(replay);
}

int main(int argc, char **argv)
{
    TEST_CHECK(argc == 2);
    test_parser(argv[1]);
    test_bulk_replay(argv[1]);
    test_control_replay(argv[1]);
    test_mismatch(argv[1]);
    return 0;
}
