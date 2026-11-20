/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#include "openst133.h"
#include "test_check.h"
#include <stdlib.h>
#include <string.h>

struct mock_handle {
    int unused;
};

static int mock_list_devices(void *userdata, ost_device_info **devices, size_t *count)
{
    const ost_device_info fixtures[] = {
        {0x1234u, 0xa010u, 1u, 1u, 0u, {0u}},
        {0x0bd7u, 0xa010u, 2u, 7u, 0u, {0u}},
        {0x0bd7u, 0x9999u, 3u, 8u, 0u, {0u}},
    };

    (void)userdata;
    *devices = malloc(sizeof(fixtures));
    if (*devices == NULL) {
        return OST_ERR_NO_MEMORY;
    }
    memcpy(*devices, fixtures, sizeof(fixtures));
    *count = sizeof(fixtures) / sizeof(fixtures[0]);
    return OST_SUCCESS;
}

static void mock_free_devices(void *userdata, ost_device_info *devices)
{
    (void)userdata;
    free(devices);
}

static int mock_open(void *userdata, const ost_device_info *info, ost_transport_handle **handle)
{
    static struct mock_handle mock;

    (void)userdata;
    (void)info;
    *handle = (ost_transport_handle *)&mock;
    return OST_SUCCESS;
}

static void mock_close(void *userdata, ost_transport_handle *handle)
{
    (void)userdata;
    (void)handle;
}

static int mock_control_transfer(
    void *userdata,
    ost_transport_handle *handle,
    uint8_t request_type,
    uint8_t request,
    uint16_t value,
    uint16_t index,
    unsigned char *data,
    uint16_t length,
    unsigned int timeout_ms,
    int *transferred)
{
    (void)userdata;
    (void)handle;
    (void)request_type;
    (void)request;
    (void)value;
    (void)index;
    (void)data;
    (void)length;
    (void)timeout_ms;
    (void)transferred;
    return OST_ERR_UNSUPPORTED;
}

static int mock_bulk_transfer(
    void *userdata,
    ost_transport_handle *handle,
    unsigned char endpoint,
    unsigned char *data,
    int length,
    unsigned int timeout_ms,
    int *transferred)
{
    (void)userdata;
    (void)handle;
    (void)endpoint;
    (void)data;
    (void)length;
    (void)timeout_ms;
    (void)transferred;
    return OST_ERR_UNSUPPORTED;
}

static ost_transport mock_transport(void)
{
    ost_transport transport = {0};

    transport.list_devices = mock_list_devices;
    transport.free_devices = mock_free_devices;
    transport.open = mock_open;
    transport.close = mock_close;
    transport.control_transfer = mock_control_transfer;
    transport.bulk_transfer = mock_bulk_transfer;
    return transport;
}

static void test_discovery_filters_st133(void)
{
    ost_transport transport = mock_transport();
    ost_device_info *devices = NULL;
    size_t count = 99u;

    TEST_CHECK(ost_list_devices(&transport, &devices, &count) == OST_SUCCESS);
    TEST_CHECK(count == 1u);
    TEST_CHECK(devices != NULL);
    TEST_CHECK(devices[0].vendor_id == 0x0bd7u);
    TEST_CHECK(devices[0].product_id == 0xa010u);
    TEST_CHECK(devices[0].bus_number == 2u);
    TEST_CHECK(devices[0].device_address == 7u);
    ost_free_devices(devices);
}

static void test_unspecified_operations_are_unsupported(void)
{
    ost_transport transport = mock_transport();
    ost_device_info *devices = NULL;
    size_t count = 0u;
    ost_device *device = NULL;
    ost_device_info info = {0};
    ost_readout_params readout = {0};
    ost_shutter_state shutter = OST_SHUTTER_CLOSED;
    unsigned char data[1] = {0};
    size_t acquired = 0u;
    double temperature = 0.0;
    uint32_t exposure_ms = 0u;
    int transferred = 0;

    TEST_CHECK(ost_list_devices(&transport, &devices, &count) == OST_SUCCESS);
    TEST_CHECK(count == 1u);
    TEST_CHECK(ost_open(&transport, &devices[0], &device) == OST_SUCCESS);
    TEST_CHECK(device != NULL);
    TEST_CHECK(ost_get_info(device, &info) == OST_SUCCESS);
    TEST_CHECK(info.vendor_id == 0x0bd7u);
    TEST_CHECK(info.product_id == 0xa010u);
    TEST_CHECK(info.bus_number == 2u);
    TEST_CHECK(info.device_address == 7u);
    TEST_CHECK(ost_get_temperature(device, &temperature) == OST_ERR_UNSUPPORTED);
    TEST_CHECK(ost_set_temperature(device, 20.0) == OST_ERR_UNSUPPORTED);
    TEST_CHECK(ost_get_shutter(device, &shutter) == OST_ERR_UNSUPPORTED);
    TEST_CHECK(ost_set_shutter(device, OST_SHUTTER_OPEN) == OST_ERR_UNSUPPORTED);
    TEST_CHECK(ost_get_exposure(device, &exposure_ms) == OST_ERR_UNSUPPORTED);
    TEST_CHECK(ost_set_exposure(device, 100u) == OST_ERR_UNSUPPORTED);
    TEST_CHECK(ost_get_readout_params(device, &readout) == OST_ERR_UNSUPPORTED);
    TEST_CHECK(ost_set_readout_params(device, &readout) == OST_ERR_UNSUPPORTED);
    TEST_CHECK(ost_acquire(device, data, sizeof(data), &acquired) == OST_ERR_UNSUPPORTED);
    TEST_CHECK(ost_abort(device) == OST_ERR_UNSUPPORTED);
    TEST_CHECK(ost_control_transfer(device, 0u, 0u, 0u, 0u, data, 0u, 0u, &transferred) == OST_ERR_UNSUPPORTED);
    TEST_CHECK(ost_bulk_transfer(device, 0u, data, 0, 0u, &transferred) == OST_ERR_UNSUPPORTED);
    ost_close(device);
    ost_free_devices(devices);
}

int main(void)
{
    test_discovery_filters_st133();
    test_unspecified_operations_are_unsupported();
    return 0;
}
