/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#include "camera_startup.h"
#include "openst133.h"
#include "protocol.h"
#include "test_check.h"
#include "transport_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CMD_SETPOINT 0x46u

struct ost_startup_data {
    unsigned char *tables[8];
};

/* Answers the startup requests like a camera whose identity reads return the selected controller type and detector. */
typedef struct identity_mock {
    unsigned int controller_type;
    unsigned int detector_id;
    unsigned char pattern[320];
    uint16_t requested;
    uint16_t series_command;
    size_t series_index;
    unsigned char written[256];
    int failing;
} identity_mock;

static const ost_device_info camera_info = {OST_USB_VENDOR_ID, OST_USB_PRODUCT_ID, 2u, 7u, 0u, {0u}};

static int mock_open(void *userdata, const ost_device_info *info, ost_transport_handle **handle)
{
    (void)userdata;
    (void)info;
    *handle = (ost_transport_handle *)(uintptr_t)1u;
    return OST_SUCCESS;
}

static void mock_close(void *userdata, ost_transport_handle *handle)
{
    (void)userdata;
    (void)handle;
}

static int mock_control(void *userdata, ost_transport_handle *handle, uint8_t request_type, uint8_t request,
    uint16_t value, uint16_t index, unsigned char *data, uint16_t length, unsigned int timeout_ms, int *transferred)
{
    (void)userdata;
    (void)handle;
    (void)request_type;
    (void)request;
    (void)value;
    (void)index;
    (void)timeout_ms;
    memset(data, 0, length);
    *transferred = length;
    return OST_SUCCESS;
}

static unsigned char reply_for(identity_mock *mock)
{
    switch (mock->requested) {
    case 0x42u:
        return OST_STARTUP_STATUS_LIVE;
    case 0x54u:
    case 0x4au:
        return mock->series_index < sizeof(mock->pattern) ? mock->pattern[mock->series_index++] : 0u;
    default:
        return 0u;
    }
}

static int mock_bulk(void *userdata, ost_transport_handle *handle, unsigned char endpoint, unsigned char *data,
    int length, unsigned int timeout_ms, int *transferred)
{
    identity_mock *mock = userdata;

    (void)handle;
    (void)timeout_ms;
    if (mock->failing) {
        return OST_ERR_IO;
    }
    if ((endpoint & 0x80u) == 0u) {
        uint16_t command = (uint16_t)(data[1] | (data[2] << 8u));

        if (command != mock->series_command) {
            mock->series_command = command;
            mock->series_index = 0u;
        }
        if (length == (int)OST_PROTO_SINGLE_COMMAND_SIZE && data[3] == OST_PROTO_RECORD_REQUEST_TAG) {
            mock->requested = command;
        } else {
            mock->written[command & 0xffu] = 1u;
            if (command == CMD_SETPOINT) {
                mock->failing = 1;
            }
        }
        *transferred = length;
        return OST_SUCCESS;
    }
    data[0] = reply_for(mock);
    data[1] = data[0];
    *transferred = length;
    return OST_SUCCESS;
}

static void mock_init(identity_mock *mock, unsigned int controller_type, unsigned int detector_id)
{
    static const unsigned char signature[5] = {0x40, 0x00, 0x42, 0x5a, 0x5a};
    static const unsigned char scale_record[6] = {0x20, 0xc0, 0x00, 0x01, 0x00, 0x02};

    memset(mock, 0, sizeof(*mock));
    mock->controller_type = controller_type;
    mock->detector_id = detector_id;
    memcpy(mock->pattern, signature, sizeof(signature));
    mock->pattern[5] = (unsigned char)(controller_type >> 8u);
    mock->pattern[6] = (unsigned char)controller_type;
    mock->pattern[11] = 0x40;
    mock->pattern[12] = 0x07;
    mock->pattern[13] = 0x00;
    mock->pattern[14] = (unsigned char)detector_id;
    memcpy(mock->pattern + 15, scale_record, sizeof(scale_record));
}

static int run_startup(identity_mock *mock, char *error, size_t error_size)
{
    ost_transport transport;
    ost_device *device = NULL;
    ost_startup_data data;
    ost_startup_options options;
    ost_startup_identity identity;
    int status;

    memset(&transport, 0, sizeof(transport));
    memset(&data, 0, sizeof(data));
    transport.userdata = mock;
    transport.open = mock_open;
    transport.close = mock_close;
    transport.control_transfer = mock_control;
    transport.bulk_transfer = mock_bulk;
    TEST_CHECK(ost_open(&transport, &camera_info, &device) == OST_SUCCESS);
    ost_startup_options_default(&options);
    options.poll_min_ms = 0u;
    options.data = &data;
    status = ost_startup_run(device, &options, &identity, error, error_size);
    ost_close(device);
    return status;
}

/* Only the writes that address the identity reads may precede the refusal: no setpoint and no write block. */
static void check_only_identity_writes(const identity_mock *mock)
{
    size_t command;

    for (command = 0u; command < sizeof(mock->written); ++command) {
        if (mock->written[command] && command != 0x30u && command != 0x40u && command != 0x54u) {
            (void)fprintf(stderr, "command 0x%02zx was written before the refusal\n", command);
            TEST_CHECK(0);
        }
    }
}

static void test_refusal(unsigned int controller_type, unsigned int detector_id, const char *expected)
{
    identity_mock mock;
    char error[256] = "";
    int status;

    mock_init(&mock, controller_type, detector_id);
    status = run_startup(&mock, error, sizeof(error));
    if (status != OST_ERR_UNSUPPORTED || strstr(error, expected) == NULL) {
        (void)fprintf(stderr, "startup returned %d: %s\n", status, error);
    }
    TEST_CHECK(status == OST_ERR_UNSUPPORTED);
    TEST_CHECK(strncmp(error, "camera not recognized: ", 23u) == 0);
    TEST_CHECK(strstr(error, expected) != NULL);
    TEST_CHECK(!mock.written[CMD_SETPOINT]);
    check_only_identity_writes(&mock);
    (void)printf("refused before any setpoint or write block: %s\n", error);
}

/* The supported identity passes the checks and reaches the setpoint write, so the refusals above are meaningful. */
static void test_supported_identity_reaches_the_setpoint(void)
{
    identity_mock mock;
    char error[256] = "";

    mock_init(&mock, OST_STARTUP_CONTROLLER_TYPE, OST_STARTUP_DETECTOR_ID_EEV_100X1340B);
    TEST_CHECK(run_startup(&mock, error, sizeof(error)) == OST_ERR_IO);
    TEST_CHECK(mock.written[CMD_SETPOINT]);
    TEST_CHECK(strstr(error, "camera not recognized") == NULL);
    (void)printf("supported identity reaches the setpoint write\n");
}

int main(void)
{
    test_refusal(7u, OST_STARTUP_DETECTOR_ID_EEV_100X1340B, "controller type 7 is unsupported (only type 10)");
    test_refusal(OST_STARTUP_CONTROLLER_TYPE, 97u, "detector ID 97 is unsupported (only 98, EEV 100x1340B)");
    test_supported_identity_reaches_the_setpoint();
    return EXIT_SUCCESS;
}
