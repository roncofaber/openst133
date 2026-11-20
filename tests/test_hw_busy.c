/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#include "openst133.h"
#include "test_check.h"
#include "transport_internal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static char fake_proc_root[PATH_MAX];
#define OST_LIBUSB_PROC_ROOT fake_proc_root
#include "../src/libusb_transport.c"
#undef OST_LIBUSB_PROC_ROOT

static const ost_device_info camera_info = {OST_USB_VENDOR_ID, OST_USB_PRODUCT_ID, 250u, 251u, 0u, {0u}};
static libusb_device *device_list[2];
static unsigned char native_device_storage[16];
static unsigned char native_handle_storage[16];
static unsigned char native_context_storage[16];
static int fake_claim_result;
static unsigned int open_calls;
static unsigned int close_calls;
static unsigned int claim_calls;
static unsigned int release_calls;
static unsigned int bulk_calls;
static unsigned int exit_calls;

static void fake_reset(void)
{
    fake_claim_result = 0;
    open_calls = 0u;
    close_calls = 0u;
    claim_calls = 0u;
    release_calls = 0u;
    bulk_calls = 0u;
    exit_calls = 0u;
    device_list[0] = (libusb_device *)(void *)native_device_storage;
    device_list[1] = NULL;
}

int LIBUSB_CALL libusb_init(libusb_context **context)
{
    *context = (libusb_context *)(void *)native_context_storage;
    return 0;
}

void LIBUSB_CALL libusb_exit(libusb_context *context)
{
    (void)context;
    ++exit_calls;
}

ssize_t LIBUSB_CALL libusb_get_device_list(libusb_context *context, libusb_device ***list)
{
    (void)context;
    *list = device_list;
    return 1;
}

void LIBUSB_CALL libusb_free_device_list(libusb_device **list, int unref_devices)
{
    (void)list;
    (void)unref_devices;
}

int LIBUSB_CALL libusb_get_device_descriptor(libusb_device *device, struct libusb_device_descriptor *descriptor)
{
    (void)device;
    memset(descriptor, 0, sizeof(*descriptor));
    descriptor->idVendor = OST_USB_VENDOR_ID;
    descriptor->idProduct = OST_USB_PRODUCT_ID;
    return 0;
}

uint8_t LIBUSB_CALL libusb_get_bus_number(libusb_device *device)
{
    (void)device;
    return camera_info.bus_number;
}

uint8_t LIBUSB_CALL libusb_get_device_address(libusb_device *device)
{
    (void)device;
    return camera_info.device_address;
}

int LIBUSB_CALL libusb_open(libusb_device *device, libusb_device_handle **handle)
{
    (void)device;
    ++open_calls;
    *handle = (libusb_device_handle *)(void *)native_handle_storage;
    return 0;
}

void LIBUSB_CALL libusb_close(libusb_device_handle *handle)
{
    (void)handle;
    ++close_calls;
}

int LIBUSB_CALL libusb_set_auto_detach_kernel_driver(libusb_device_handle *handle, int enable)
{
    (void)handle;
    (void)enable;
    return 0;
}

int LIBUSB_CALL libusb_claim_interface(libusb_device_handle *handle, int interface_number)
{
    (void)handle;
    TEST_CHECK(interface_number == 0);
    ++claim_calls;
    return fake_claim_result;
}

int LIBUSB_CALL libusb_release_interface(libusb_device_handle *handle, int interface_number)
{
    (void)handle;
    TEST_CHECK(interface_number == 0);
    ++release_calls;
    return 0;
}

int LIBUSB_CALL libusb_bulk_transfer(libusb_device_handle *handle, unsigned char endpoint, unsigned char *data,
    int length, int *actual_length, unsigned int timeout)
{
    (void)handle;
    (void)endpoint;
    (void)data;
    (void)timeout;
    ++bulk_calls;
    *actual_length = length;
    return 0;
}

int LIBUSB_CALL libusb_control_transfer(libusb_device_handle *handle, uint8_t request_type, uint8_t request,
    uint16_t value, uint16_t index, unsigned char *data, uint16_t length, unsigned int timeout)
{
    (void)handle;
    (void)request_type;
    (void)request;
    (void)value;
    (void)index;
    (void)data;
    (void)length;
    (void)timeout;
    return 0;
}

struct libusb_transfer *LIBUSB_CALL libusb_alloc_transfer(int iso_packets)
{
    (void)iso_packets;
    return calloc(1u, sizeof(struct libusb_transfer));
}

int LIBUSB_CALL libusb_submit_transfer(struct libusb_transfer *transfer)
{
    (void)transfer;
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALL libusb_cancel_transfer(struct libusb_transfer *transfer)
{
    (void)transfer;
    return 0;
}

void LIBUSB_CALL libusb_free_transfer(struct libusb_transfer *transfer)
{
    free(transfer);
}

int LIBUSB_CALL libusb_handle_events_timeout_completed(libusb_context *context, struct timeval *timeout, int *completed)
{
    (void)context;
    (void)timeout;
    (void)completed;
    return 0;
}

static void make_holder(long pid, const char *name, const char *device_path, int descriptor)
{
    char path[PATH_MAX];
    FILE *file;

    TEST_CHECK(snprintf(path, sizeof(path), "%s/%ld", fake_proc_root, pid) > 0);
    TEST_CHECK(mkdir(path, 0700) == 0);
    TEST_CHECK(snprintf(path, sizeof(path), "%s/%ld/comm", fake_proc_root, pid) > 0);
    file = fopen(path, "wb");
    TEST_CHECK(file != NULL);
    TEST_CHECK(fprintf(file, "%s\n", name) > 0);
    TEST_CHECK(fclose(file) == 0);
    TEST_CHECK(snprintf(path, sizeof(path), "%s/%ld/fd", fake_proc_root, pid) > 0);
    TEST_CHECK(mkdir(path, 0700) == 0);
    TEST_CHECK(snprintf(path, sizeof(path), "%s/%ld/fd/%d", fake_proc_root, pid, descriptor) > 0);
    TEST_CHECK(symlink(device_path, path) == 0);
}

static void test_claim_failure(int native_error, int expected_status, const char *expected_text)
{
    ost_device *device = NULL;
    int status;

    fake_reset();
    fake_claim_result = native_error;
    status = ost_open(ost_libusb_transport(), &camera_info, &device);
    TEST_CHECK(device == NULL);
    TEST_CHECK(status == expected_status);
    TEST_CHECK(open_calls == 1u);
    TEST_CHECK(claim_calls == 1u);
    TEST_CHECK(bulk_calls == 0u);
    TEST_CHECK(close_calls == 1u);
    TEST_CHECK(release_calls == 0u);
    TEST_CHECK(exit_calls == 1u);
    TEST_CHECK(strstr(ost_open_error(), expected_text) != NULL);
    TEST_CHECK(strstr(ost_open_error(), "/dev/bus/usb/250/251") != NULL);
}

static void test_busy_without_holder(void)
{
    test_claim_failure(LIBUSB_ERROR_BUSY, OST_ERR_BUSY, "busy");
    TEST_CHECK(strcmp(ost_status_text(OST_ERR_BUSY), "device is busy") == 0);
    TEST_CHECK(strstr(ost_open_error(), "pid ") == NULL);
}

static void test_access_denied(void)
{
    test_claim_failure(LIBUSB_ERROR_ACCESS, OST_ERR_ACCESS, "permission denied");
    TEST_CHECK(strcmp(ost_status_text(OST_ERR_ACCESS), "permission denied") == 0);
    TEST_CHECK(strstr(ost_open_error(), "udev") != NULL);
}

static void test_self_is_not_the_holder(void)
{
    make_holder((long)getpid(), "ost-self", "/dev/bus/usb/250/251", 3);
    test_claim_failure(LIBUSB_ERROR_BUSY, OST_ERR_BUSY, "busy");
    TEST_CHECK(strstr(ost_open_error(), "pid ") == NULL);
}

static void test_usbfs_holder(void)
{
    make_holder(424242L, "qemu-system-x86_64", "/dev/bus/usb/250/251", 8);
    test_claim_failure(LIBUSB_ERROR_BUSY, OST_ERR_BUSY, "busy");
    TEST_CHECK(strstr(ost_open_error(), "424242") != NULL);
    TEST_CHECK(strstr(ost_open_error(), "qemu-system-x86_64") != NULL);
}

static void test_normal_open(void)
{
    ost_device *device = NULL;
    unsigned char data = 0x5au;
    int moved = 0;

    fake_reset();
    TEST_CHECK(ost_open(ost_libusb_transport(), &camera_info, &device) == OST_SUCCESS);
    TEST_CHECK(ost_open_error()[0] == '\0');
    TEST_CHECK(device != NULL);
    TEST_CHECK(open_calls == 1u);
    TEST_CHECK(claim_calls == 1u);
    TEST_CHECK(bulk_calls == 0u);
    TEST_CHECK(ost_bulk_transfer(device, 0x08u, &data, 1, 100u, &moved) == OST_SUCCESS);
    TEST_CHECK(moved == 1);
    TEST_CHECK(bulk_calls == 1u);
    TEST_CHECK(claim_calls == 1u);
    ost_close(device);
    TEST_CHECK(release_calls == 1u);
    TEST_CHECK(close_calls == 1u);
    TEST_CHECK(exit_calls == 1u);
}

int main(int argc, char **argv)
{
    char directory[PATH_MAX];
    char path[PATH_MAX];

    TEST_CHECK(argc == 2);
    TEST_CHECK(test_unsetenv(OST_NO_HARDWARE_ENV) == 0);
    TEST_CHECK(snprintf(directory, sizeof(directory), "%s/hw-busy-XXXXXX", argv[1]) > 0);
    TEST_CHECK(test_mkdtemp(directory) != NULL);
    TEST_CHECK(snprintf(fake_proc_root, sizeof(fake_proc_root), "%s/proc", directory) > 0);
    TEST_CHECK(mkdir(fake_proc_root, 0700) == 0);
    TEST_CHECK(snprintf(path, sizeof(path), "%s/proc/not-a-pid", directory) > 0);
    TEST_CHECK(mkdir(path, 0700) == 0);
    test_busy_without_holder();
    test_access_denied();
    test_self_is_not_the_holder();
    test_usbfs_holder();
    test_normal_open();
    puts("claim failures preserve cause and holder without bulk transfers; successful opens still transfer");
    return 0;
}
