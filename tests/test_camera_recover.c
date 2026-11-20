/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#include "camera_recover.h"
#include "test_check.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum operation_kind {
    OPERATION_IMAGE_DATA,
    OPERATION_IMAGE_TIMEOUT,
    OPERATION_QUERY,
    OPERATION_WRITE
} operation_kind;

typedef struct expected_operation {
    operation_kind kind;
    uint16_t command;
    uint16_t argument;
    unsigned char response;
} expected_operation;

typedef struct recovery_script {
    ost_transport transport;
    ost_device_info info;
    expected_operation operations[12];
    size_t count;
    size_t position;
    int query_half;
    int read_first;
    char error[256];
} recovery_script;

static int script_list(void *userdata, ost_device_info **devices, size_t *count)
{
    recovery_script *script = userdata;

    *devices = malloc(sizeof(**devices));
    TEST_CHECK(*devices != NULL);
    **devices = script->info;
    *count = 1u;
    return OST_SUCCESS;
}

static void script_free(void *userdata, ost_device_info *devices)
{
    (void)userdata;
    free(devices);
}

static int script_open(void *userdata, const ost_device_info *info, ost_transport_handle **handle)
{
    recovery_script *script = userdata;

    TEST_CHECK(info->bus_number == script->info.bus_number && info->device_address == script->info.device_address);
    *handle = (ost_transport_handle *)script;
    return OST_SUCCESS;
}

static void script_close(void *userdata, ost_transport_handle *handle)
{
    (void)userdata;
    (void)handle;
}

static void script_fail(recovery_script *script, const char *reason, unsigned char endpoint, int length)
{
    if (script->error[0] == '\0') {
        (void)snprintf(script->error, sizeof(script->error), "operation %zu: %s (endpoint 0x%02x, length %d)",
            script->position, reason, endpoint, length);
    }
}

static int script_bulk(void *userdata, ost_transport_handle *handle, unsigned char endpoint,
    unsigned char *data, int length, unsigned int timeout_ms, int *transferred)
{
    recovery_script *script = userdata;
    const expected_operation *operation;
    unsigned char expected[6];
    size_t i;

    (void)handle;
    (void)timeout_ms;
    *transferred = 0;
    if (script->position >= script->count) {
        script_fail(script, "unexpected extra transfer", endpoint, length);
        return OST_ERR_IO;
    }
    operation = &script->operations[script->position];
    if (operation->kind == OPERATION_IMAGE_DATA || operation->kind == OPERATION_IMAGE_TIMEOUT) {
        if (endpoint != 0x82u || length != 20480) {
            script_fail(script, "wrong image drain transfer", endpoint, length);
            return OST_ERR_IO;
        }
        ++script->position;
        if (operation->kind == OPERATION_IMAGE_TIMEOUT) {
            return OST_ERR_TIMEOUT;
        }
        for (i = 0u; i < 32u; ++i) {
            data[i] = (unsigned char)(0x80u + i);
        }
        *transferred = 32;
        return OST_SUCCESS;
    }
    if (operation->kind == OPERATION_QUERY) {
        int response_turn = script->read_first ? script->query_half == 0 : script->query_half != 0;

        expected[0] = 0x01u;
        expected[1] = (unsigned char)(operation->command & 0xffu);
        expected[2] = (unsigned char)(operation->command >> 8u);
        expected[3] = 0x03u;
        expected[4] = 0u;
        expected[5] = 0u;
        if (response_turn) {
            if (endpoint != 0x86u || length != 2) {
                script_fail(script, "query response is out of order", endpoint, length);
                return OST_ERR_IO;
            }
            data[0] = operation->response;
            data[1] = operation->response;
            *transferred = 2;
        } else {
            if (endpoint != 0x08u || length != 6 || memcmp(data, expected, sizeof(expected)) != 0) {
                script_fail(script, "wrong query request", endpoint, length);
                return OST_ERR_IO;
            }
            *transferred = length;
        }
        if (script->query_half != 0) {
            script->query_half = 0;
            ++script->position;
        } else {
            script->query_half = 1;
        }
        return OST_SUCCESS;
    }
    expected[0] = 0x01u;
    expected[1] = (unsigned char)(operation->command & 0xffu);
    expected[2] = (unsigned char)(operation->command >> 8u);
    expected[3] = 0x02u;
    expected[4] = (unsigned char)(operation->argument & 0xffu);
    expected[5] = (unsigned char)(operation->argument >> 8u);
    if (endpoint != 0x08u || length != 6 || memcmp(data, expected, sizeof(expected)) != 0) {
        script_fail(script, "wrong stop write", endpoint, length);
        return OST_ERR_IO;
    }
    *transferred = length;
    ++script->position;
    return OST_SUCCESS;
}

static void script_init(recovery_script *script, int read_first, int persistent)
{
    static const expected_operation tail[] = {
        {OPERATION_QUERY, 0x30u, 0u, 0x00u},
        {OPERATION_WRITE, 0x30u, 2u, 0u},
        {OPERATION_WRITE, 0x30u, 0u, 0u},
        {OPERATION_WRITE, 0x30u, 1u, 0u},
        {OPERATION_WRITE, 0x40u, 150u, 0u},
        {OPERATION_WRITE, 0x40u, 149u, 0u},
        {OPERATION_WRITE, 0x30u, 0u, 0u},
        {OPERATION_WRITE, 0x30u, 1u, 0u},
        {OPERATION_WRITE, 0x30u, 3u, 0u}
    };

    memset(script, 0, sizeof(*script));
    script->info.vendor_id = OST_USB_VENDOR_ID;
    script->info.product_id = OST_USB_PRODUCT_ID;
    script->info.bus_number = 1u;
    script->info.device_address = 2u;
    script->read_first = read_first;
    script->operations[0].kind = OPERATION_IMAGE_DATA;
    script->operations[1].kind = OPERATION_IMAGE_TIMEOUT;
    memcpy(script->operations + 2u, tail, sizeof(tail));
    script->operations[11].kind = OPERATION_QUERY;
    script->operations[11].command = 0x42u;
    script->operations[11].response = persistent ? OST_CAMERA_STATUS_ACQUIRING : 0xa7u;
    script->count = 12u;
    script->transport.userdata = script;
    script->transport.list_devices = script_list;
    script->transport.free_devices = script_free;
    script->transport.open = script_open;
    script->transport.close = script_close;
    script->transport.bulk_transfer = script_bulk;
}

static void run_recovery(int read_first, int persistent)
{
    recovery_script script;
    ost_device *device = NULL;
    unsigned char status = 0u;
    char error[256];
    int result;

    script_init(&script, read_first, persistent);
    TEST_CHECK(ost_open(&script.transport, &script.info, &device) == OST_SUCCESS);
    result = ost_camera_recover(device, read_first, OST_CAMERA_STATUS_ACQUIRING, &status, error, sizeof(error));
    if (persistent) {
        TEST_CHECK(result == OST_ERR_IO);
        TEST_CHECK(status == OST_CAMERA_STATUS_ACQUIRING);
        TEST_CHECK(strstr(error, "0xa5") != NULL);
    } else {
        TEST_CHECK(result == OST_SUCCESS);
        TEST_CHECK(status == 0xa7u);
    }
    TEST_CHECK(script.error[0] == '\0');
    TEST_CHECK(script.position == script.count);
    TEST_CHECK(script.query_half == 0);
    ost_close(device);
}

static void test_no_unneeded_recovery(void)
{
    recovery_script script;
    ost_device *device = NULL;
    unsigned char status = 0u;
    char error[256];

    script_init(&script, 1, 0);
    TEST_CHECK(ost_open(&script.transport, &script.info, &device) == OST_SUCCESS);
    TEST_CHECK(ost_camera_recover(device, 1, 0xa7u, &status, error, sizeof(error)) == OST_SUCCESS);
    TEST_CHECK(status == 0xa7u);
    TEST_CHECK(ost_camera_recover(device, 1, 0x63u, &status, error, sizeof(error)) == OST_SUCCESS);
    TEST_CHECK(status == 0x63u);
    TEST_CHECK(script.position == 0u);
    TEST_CHECK(script.error[0] == '\0');
    ost_close(device);
}

int main(void)
{
    run_recovery(1, 0);
    run_recovery(0, 0);
    run_recovery(1, 1);
    test_no_unneeded_recovery();
    (void)printf("0xa5 drain, stop and one status retry passed in both transfer orders; persistent 0xa5 failed\n");
    return 0;
}
