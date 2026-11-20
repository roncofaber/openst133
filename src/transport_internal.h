/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#ifndef OPENST133_TRANSPORT_INTERNAL_H
#define OPENST133_TRANSPORT_INTERNAL_H

#include "openst133.h"

/*
 * Transport status for an operation that hit its timeout. It extends ost_status
 * so callers can tell a timeout from other I/O errors.
 */
#define OST_TRANSPORT_ERR_TIMEOUT (-100)

const ost_transport *ost_libusb_transport(void);

/* Detail for the most recent failed libusb open in this thread; empty when none is available. */
const char *ost_libusb_open_error(void);

/*
 * Exchange the transport and handle of an open device with the given ones. Used to interpose on a device for the
 * duration of one operation; calling it again with the returned values restores the device.
 */
int ost_device_swap_transport(ost_device *device, ost_transport *transport, ost_transport_handle **handle);

#endif
