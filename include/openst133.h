/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#ifndef OPENST133_H
#define OPENST133_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** USB identity of the ST-133 interface. */
#define OST_USB_VENDOR_ID 0x0bd7u
#define OST_USB_PRODUCT_ID 0xa010u

/** Maximum USB port-chain depth in ost_device_info (the USB 3.0 limit of libusb_get_port_numbers). */
#define OST_MAX_PORT_DEPTH 7u

/**
 * Status returned by openst133 operations.
 *
 * OST_SUCCESS: the operation completed.
 * OST_ERR_INVALID_ARGUMENT: a NULL pointer, zero size or out-of-range value; nothing was sent.
 * OST_ERR_NO_MEMORY: an allocation failed.
 * OST_ERR_IO: a transport or protocol failure, including a short transfer.
 * OST_ERR_NOT_FOUND: no such device, or the device disappeared.
 * OST_ERR_UNSUPPORTED: the operation, value or camera is not supported on this
 *   transport, and nothing was sent. The camera's last-error text
 *   names the reason.
 * OST_ERR_ABORTED: the operation was interrupted by ost_camera_abort.
 * OST_ERR_BUFFER_TOO_SMALL: the caller buffer cannot hold the result; nothing was sent.
 * OST_ERR_TIMEOUT: a transfer timed out.
 * OST_ERR_BUSY: another process or driver holds the device interface.
 * OST_ERR_ACCESS: the operating system denied access to the device interface.
 * OST_ERR_CLOSED: the camera handle was closed, or is being closed by another thread.
 */
typedef enum ost_status {
    OST_SUCCESS = 0,
    OST_ERR_INVALID_ARGUMENT = -1,
    OST_ERR_NO_MEMORY = -2,
    OST_ERR_IO = -3,
    OST_ERR_NOT_FOUND = -4,
    OST_ERR_UNSUPPORTED = -5,
    OST_ERR_ABORTED = -6,
    OST_ERR_BUFFER_TOO_SMALL = -7,
    OST_ERR_TIMEOUT = -8,
    OST_ERR_BUSY = -9,
    OST_ERR_ACCESS = -10,
    OST_ERR_CLOSED = -11
} ost_status;

/** Short text for a status value; never NULL. */
const char *ost_status_text(int status);

/** An opened device; its contents are private to the library. */
typedef struct ost_device ost_device;

/** A backend-specific opened handle; its contents are private to the backend. */
typedef struct ost_transport_handle ost_transport_handle;

/**
 * Transport-neutral USB identity; no protocol capabilities are implied.
 *
 * device_address is reassigned by the host on every reconnection. The port
 * path (port_numbers[0..port_count-1], from the root hub down) names the
 * physical socket and survives reconnection to the same socket; port_count is
 * zero when the transport cannot report it (for example, a replay transport).
 */
typedef struct ost_device_info {
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t bus_number;
    uint8_t device_address;
    uint8_t port_count;
    uint8_t port_numbers[OST_MAX_PORT_DEPTH];
} ost_device_info;

/**
 * Shutter mode of the next acquisition. OST_SHUTTER_OPEN is WinSpec's Normal mode (open during the exposure),
 * OST_SHUTTER_CLOSED Disabled Closed, OST_SHUTTER_DISABLED_OPENED Disabled Opened and OST_SHUTTER_PRE_OPEN the Pre Open
 * option, which WinSpec offers only with External Sync timing. The device-level ost_set_shutter maps none of them.
 */
typedef enum ost_shutter_state {
    OST_SHUTTER_CLOSED = 0,
    OST_SHUTTER_OPEN = 1,
    OST_SHUTTER_DISABLED_OPENED = 2,
    OST_SHUTTER_PRE_OPEN = 3
} ost_shutter_state;

/** Transport-neutral readout request; no default values are implied. */
typedef struct ost_readout_params {
    uint32_t region_x;
    uint32_t region_y;
    uint32_t region_width;
    uint32_t region_height;
    uint32_t x_binning;
    uint32_t y_binning;
    uint32_t readout_rate_hz;
} ost_readout_params;

/**
 * Pluggable byte-transport interface.
 *
 * list_devices returns an allocated array containing all visible USB devices,
 * not only ST-133 devices. free_devices releases that array. open and close
 * manage backend handles. Transfer callbacks carry caller-supplied fields
 * unchanged and do not assign any ST-133 protocol meaning to them. All
 * callbacks return an ost_status value. The structure and userdata must remain
 * valid for the duration of a call; ost_open copies the structure.
 */
typedef struct ost_transport {
    void *userdata;
    int (*list_devices)(void *userdata, ost_device_info **devices, size_t *count);
    void (*free_devices)(void *userdata, ost_device_info *devices);
    int (*open)(void *userdata, const ost_device_info *info, ost_transport_handle **handle);
    void (*close)(void *userdata, ost_transport_handle *handle);
    int (*control_transfer)(
        void *userdata,
        ost_transport_handle *handle,
        uint8_t request_type,
        uint8_t request,
        uint16_t value,
        uint16_t index,
        unsigned char *data,
        uint16_t length,
        unsigned int timeout_ms,
        int *transferred);
    int (*bulk_transfer)(
        void *userdata,
        ost_transport_handle *handle,
        unsigned char endpoint,
        unsigned char *data,
        int length,
        unsigned int timeout_ms,
        int *transferred);
} ost_transport;

/**
 * Discover ST-133 interfaces with VID/PID 0x0bd7/0xa010.
 *
 * A NULL transport selects libusb. On success, count entries are returned in
 * an array owned by the caller; an empty result has devices == NULL. Release
 * the array with ost_free_devices. Discovery does not open USB devices.
 *
 * Entries are sorted by bus number, then port path (a shorter path that is a
 * prefix of a longer one comes first), then device address, whatever order the
 * transport reports, so an index into the result names the same socket while
 * the USB topology is unchanged.
 */
int ost_list_devices(const ost_transport *transport, ost_device_info **devices, size_t *count);

/** Free an array returned by ost_list_devices; NULL is accepted. */
void ost_free_devices(ost_device_info *devices);

/**
 * Write the port path of info as BUS-PORT[.PORT...] (for example "1-4.2", the
 * Linux sysfs device name) into buffer. Returns OST_ERR_UNSUPPORTED with an
 * empty buffer when port_count is zero, OST_ERR_BUFFER_TOO_SMALL when the text
 * does not fit, and OST_ERR_INVALID_ARGUMENT for a NULL argument, a zero size or
 * a port_count above OST_MAX_PORT_DEPTH.
 */
int ost_device_port_path(const ost_device_info *info, char *buffer, size_t size);

/**
 * Choose one entry of devices (as returned by ost_list_devices) and store its
 * position in index. selector is one of:
 *
 *   N           the decimal position N in the list, counted from 0;
 *   BUS-P[.P]   the port path, as written by ost_device_port_path;
 *   BUS:ADDR    the decimal bus number and current device address.
 *
 * A NULL or empty selector chooses the only device. OST_ERR_NOT_FOUND means no
 * device (or no device matching the selector); OST_ERR_INVALID_ARGUMENT means a
 * malformed selector, a NULL argument, or several devices and no selector,
 * which never picks one silently. No device is opened.
 */
int ost_select_device(const ost_device_info *devices, size_t count, const char *selector, size_t *index);

/**
 * Open the exact bus/address described by info; when info carries a port path
 * the device must also be at that path.
 *
 * A NULL transport selects libusb. The device information is copied. A
 * non-ST-133 identity returns OST_ERR_NOT_FOUND. No camera command is sent.
 */
int ost_open(const ost_transport *transport, const ost_device_info *info, ost_device **device);

/**
 * Text describing why this thread's most recent ost_open failed, such as a busy interface and its holder or a
 * permission problem; never NULL, empty after a successful open.
 */
const char *ost_open_error(void);

/** Close a device and release its resources; NULL is accepted. */
void ost_close(ost_device *device);

/** Return the USB descriptor cached by ost_open; no I/O is performed. */
int ost_get_info(ost_device *device, ost_device_info *info);

/** Read temperature in degrees Celsius; currently returns OST_ERR_UNSUPPORTED. */
int ost_get_temperature(ost_device *device, double *celsius);

/** Set temperature in degrees Celsius; currently returns OST_ERR_UNSUPPORTED. */
int ost_set_temperature(ost_device *device, double celsius);

/** Read shutter state; currently returns OST_ERR_UNSUPPORTED. */
int ost_get_shutter(ost_device *device, ost_shutter_state *state);

/** Set shutter state; currently returns OST_ERR_UNSUPPORTED. */
int ost_set_shutter(ost_device *device, ost_shutter_state state);

/** Read exposure duration in milliseconds; currently returns OST_ERR_UNSUPPORTED. */
int ost_get_exposure(ost_device *device, uint32_t *exposure_ms);

/** Set exposure duration in milliseconds; currently returns OST_ERR_UNSUPPORTED. */
int ost_set_exposure(ost_device *device, uint32_t exposure_ms);

/** Read readout parameters; currently returns OST_ERR_UNSUPPORTED. */
int ost_get_readout_params(ost_device *device, ost_readout_params *params);

/** Set readout parameters; currently returns OST_ERR_UNSUPPORTED. */
int ost_set_readout_params(ost_device *device, const ost_readout_params *params);

/**
 * Acquire bytes into caller-owned storage; currently returns
 * OST_ERR_UNSUPPORTED. No buffer format or capacity rule is implied.
 */
int ost_acquire(ost_device *device, void *buffer, size_t buffer_size, size_t *acquired_size);

/** Abort acquisition; currently returns OST_ERR_UNSUPPORTED. */
int ost_abort(ost_device *device);

/**
 * Forward one generic USB control transfer through the selected backend.
 * transferred receives the byte count on success. No protocol semantics are
 * attached to the arguments; a backend without this hook returns
 * OST_ERR_UNSUPPORTED.
 */
int ost_control_transfer(
    ost_device *device,
    uint8_t request_type,
    uint8_t request,
    uint16_t value,
    uint16_t index,
    unsigned char *data,
    uint16_t length,
    unsigned int timeout_ms,
    int *transferred);

/**
 * Forward one generic USB bulk transfer through the selected backend.
 * transferred receives the byte count on success. No protocol semantics are
 * attached to the arguments; a backend without this hook returns
 * OST_ERR_UNSUPPORTED.
 */
int ost_bulk_transfer(
    ost_device *device,
    unsigned char endpoint,
    unsigned char *data,
    int length,
    unsigned int timeout_ms,
    int *transferred);


/*
 * Camera API.
 *
 * A camera owns one opened device. On the USB transport (a NULL or libusb transport) opening runs the camera
 * startup: identification reads, then the five setup write blocks from the startup data file. The startup refuses
 * with OST_ERR_UNSUPPORTED, before any setpoint write or write block, unless the identity reads carry the ZZ
 * signature, controller type 10 and detector 98 (EEV 100x1340B); ost_camera_open_error then starts with
 * "camera not recognized: ". After open, setpoint, exposure, shutter, gain, accumulations, readout rate and
 * ROI/binning exchanges are built from the requested values. An unsupported value or combination returns
 * OST_ERR_UNSUPPORTED, names the unsupported field in the last error and sends nothing.
 *
 * On any other transport (a replay or mock transport) the camera drives the generic protocol path used by tests and
 * the replay diagnostic: no startup runs, exposure and readout are stored on the host and acquisition reads a frame
 * of the selected size. Temperature, setpoint, shutter, gain, accumulations, readout rate and streaming return
 * OST_ERR_UNSUPPORTED there.
 *
 * Threading. A camera handle may be shared between threads without external locking:
 *
 * - Operations that talk to the device or change settings (the temperature calls, the setters, ost_camera_acquire
 *   and ost_camera_stream) run one at a time per camera. A call made while another thread
 *   runs one blocks until it returns, so a temperature read issued during ost_camera_stream waits until the stream
 *   ends; a caller that must not block should skip such calls while it acquires. Different cameras never block each
 *   other.
 * - Host-side getters (ost_camera_get_target_source, _get_geometry, _get_identity, _get_exposure,
 *   _get_effective_exposure, _get_readout, _get_shutter, _get_gain, _get_accumulations, _query_support) and
 *   ost_camera_abort never wait for a running operation.
 * - A frame callback runs on the thread of the acquiring call and may call any camera function on that camera,
 *   including ost_camera_abort and ost_camera_close; operations called from it run immediately, nested inside the
 *   acquisition.
 * - ost_camera_close may be called from any thread at any time. It requests an abort, wakes every thread waiting
 *   to start an operation (those calls return OST_ERR_CLOSED), waits until the running operation has returned and
 *   then releases the camera. Called from a frame callback it returns at once and the camera is released when the
 *   acquisition below the callback returns. A second close of the same handle does nothing.
 * - Every camera call with a closed handle, including one that was waiting while close ran, returns OST_ERR_CLOSED
 *   (ost_camera_last_error returns text saying so) and does not touch freed memory. The handle value stays
 *   invalid; it may compare equal to a camera opened later, which then receives the call.
 * - ost_camera_last_error reports the calling thread's most recent operation on that camera, so concurrent threads
 *   never see each other's text.
 */
typedef struct ost_camera ost_camera;

/** Source of the target written by startup. */
typedef enum ost_camera_target_source {
    OST_CAMERA_TARGET_SOURCE_NONE = 0,
    OST_CAMERA_TARGET_SOURCE_OPTION = 1,
    OST_CAMERA_TARGET_SOURCE_PERSISTED = 2,
    OST_CAMERA_TARGET_SOURCE_CAMERA_DEFAULT = 3,
    OST_CAMERA_TARGET_SOURCE_STARTUP_DEFAULT = 4
} ost_camera_target_source;

/** Optional open settings; target_set is nonzero to override the persisted and camera defaults. */
typedef struct ost_camera_open_options {
    int target_set;
    double target_celsius;
} ost_camera_open_options;

void ost_camera_open_options_default(ost_camera_open_options *options);
/** Default sensor geometry before the camera identifies itself. */
#define OST_CAMERA_DEFAULT_WIDTH 1340u
#define OST_CAMERA_DEFAULT_HEIGHT 100u
/** Bytes per pixel of a frame (16-bit samples). */
#define OST_CAMERA_BYTES_PER_PIXEL 2u

/** Sensor geometry; reported is 0 while the values are the defaults. */
typedef struct ost_sensor_geometry {
    uint32_t width;
    uint32_t height;
    int reported;
} ost_sensor_geometry;

/** A sensor ROI and the integer binning applied within it. */
typedef struct ost_camera_readout {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t x_binning;
    uint32_t y_binning;
} ost_camera_readout;

/** Identification decoded during startup. */
typedef struct ost_camera_identity {
    uint32_t controller_type;
    uint32_t detector_id;
    char detector_name[32];
    uint32_t width;
    uint32_t height;
    /* Nonzero when startup supplied a target from an option, persistence or camera metadata. */
    int target_known;
    double target_celsius;
} ost_camera_identity;

/**
 * Open a camera on the device described by info. A NULL or libusb transport opens the camera on USB and runs its
 * startup (see above); any other transport selects the generic protocol path and sends no startup. Returns
 * OST_ERR_NOT_FOUND for a non-ST-133 identity and OST_ERR_UNSUPPORTED for an unrecognized camera. To check USB access
 * without camera commands, use ost_open and ost_close.
 */
int ost_camera_open(const ost_transport *transport, const ost_device_info *info, ost_camera **camera);

/**
 * As ost_camera_open with open options. Startup chooses the target from an explicit option, the persisted target for
 * this USB device, the camera metadata default, or the -120 C startup default, in that order. The persisted target is
 * stored per port path when info carries one (so it follows the socket across reconnection), and per bus and address
 * otherwise. target_set on a transport other than USB returns OST_ERR_INVALID_ARGUMENT.
 */
int ost_camera_open_with_options(
    const ost_transport *transport,
    const ost_device_info *info,
    const ost_camera_open_options *options,
    ost_camera **camera);

/**
 * Text describing why the most recent ost_camera_open or ost_camera_open_with_options call of this
 * thread failed; never NULL, empty when it succeeded. A failed open returns no
 * camera, so its last-error text is not otherwise reachable.
 */
const char *ost_camera_open_error(void);

/**
 * Close a camera and its device; NULL and an already closed handle are accepted. No shutdown command is sent. A
 * running acquisition or stream is aborted (its stop exchange is sent by the acquiring thread) and close returns
 * after it has ended; see the threading notes above.
 */
void ost_camera_close(ost_camera *camera);

/** Camera operations whose availability ost_camera_query_support reports. */
typedef enum ost_camera_operation {
    OST_CAMERA_OP_GET_TEMPERATURE = 0,
    OST_CAMERA_OP_SET_TEMPERATURE = 1,
    OST_CAMERA_OP_SET_EXPOSURE = 2,
    OST_CAMERA_OP_ACQUIRE = 3,
    OST_CAMERA_OP_ABORT = 4,
    OST_CAMERA_OP_SET_READOUT = 5
} ost_camera_operation;

/**
 * Report without any I/O whether an operation is available on this camera's
 * transport: OST_SUCCESS, OST_ERR_UNSUPPORTED, or OST_ERR_INVALID_ARGUMENT for an
 * unknown operation.
 */
int ost_camera_query_support(const ost_camera *camera, ost_camera_operation operation);

/** Source of the target written during open; no I/O. NONE before identification. */
int ost_camera_get_target_source(const ost_camera *camera, ost_camera_target_source *source);

/**
 * Text describing the failure of the calling thread's most recent operation on this camera; never NULL, empty after
 * a success or when this thread has not used the camera. The text stays valid until this thread's next camera call.
 */
const char *ost_camera_last_error(const ost_camera *camera);

/**
 * Sensor geometry; never sends anything. Defaults to 1340x100 with reported == 0;
 * startup identification sets reported == 1 from the decoded chip ID.
 */
int ost_camera_get_geometry(const ost_camera *camera, ost_sensor_geometry *geometry);

/**
 * Identification decoded during startup (chip ID 98 is EEV 100x1340B,
 * 1340x100). OST_ERR_UNSUPPORTED on a replay or mock transport; never sends anything.
 */
int ost_camera_get_identity(const ost_camera *camera, ost_camera_identity *identity);

/**
 * Read the sensor temperature in Celsius: one status request 0x42 followed by
 * eleven 0x46 requests, with the first sample discarded. On a replay or mock
 * transport OST_ERR_UNSUPPORTED and nothing is sent. Status 0xa7 selects the temperature
 * conversion; any other status returns OST_ERR_UNSUPPORTED. The value is rounded
 * to half-degree steps.
 */
int ost_camera_get_temperature(ost_camera *camera, double *celsius);

/**
 * Set the temperature setpoint in Celsius, -152 C to -25 C in 0.5 C steps; a value outside that range returns
 * OST_ERR_INVALID_ARGUMENT with nothing sent. A successful write also saves the target for the next open of this USB
 * device.
 * A persistence failure returns OST_ERR_IO after the camera write and names that distinction in the last error.
 */
int ost_camera_set_temperature(ost_camera *camera, double celsius);

/**
 * Set the programmed integration exposure in milliseconds (nonzero). This is not the total acquisition time:
 * mechanical shutter opening and closing compensation and the subsequent readout are additional. In Disabled Opened
 * operation the detector can continue collecting light outside this programmed interval; the value is still the
 * exposure request, not a measurement of optical integration. Stored for the next acquisition; nothing is sent.
 * On USB, 0 selects the bias exposure and values above 60000 ms return OST_ERR_UNSUPPORTED; the exposure is encoded as
 * count * 2^scale ms with count at most 255 (see ost_camera_get_effective_exposure). A replay or mock transport
 * refuses 0 with OST_ERR_INVALID_ARGUMENT.
 */
int ost_camera_set_exposure(ost_camera *camera, uint32_t exposure_ms);

/** Exposure last set, in milliseconds; 0 when never set. Host-side, always available. */
int ost_camera_get_exposure(const ost_camera *camera, uint32_t *exposure_ms);

/** Effective exposure in milliseconds after generated count truncation; 0 when never set. Host-side. */
int ost_camera_get_effective_exposure(const ost_camera *camera, uint32_t *exposure_ms);

/**
 * Select the shutter for the next acquisition: OST_SHUTTER_OPEN selects Normal mode (0x3a = 6),
 * OST_SHUTTER_CLOSED selects Disabled Closed mode (0x3a = 4) and OST_SHUTTER_DISABLED_OPENED selects Disabled
 * Opened (0x3a = 0x86). OST_SHUTTER_PRE_OPEN returns OST_ERR_UNSUPPORTED because External Sync timing is
 * unsupported. Nothing is sent until ost_camera_acquire.
 */
int ost_camera_set_shutter(ost_camera *camera, ost_shutter_state state);

/** Shutter mode last selected (OST_SHUTTER_OPEN by default); host-side, never sends anything. */
int ost_camera_get_shutter(const ost_camera *camera, ost_shutter_state *state);

/**
 * Select WinSpec's Controller Gain 1, 2 or 3 for the next acquisition (the 16-bit gain word of command 0x4a:
 * 0x0100, 0x0140, 0x0180). Gain 0 is OST_ERR_INVALID_ARGUMENT, a gain above 3 OST_ERR_UNSUPPORTED naming the
 * field. Nothing is sent until ost_camera_acquire.
 */
int ost_camera_set_gain(ost_camera *camera, uint32_t gain);

/** Controller gain of the next acquisition; 2 (the gain startup stores) until set. Host-side. */
int ost_camera_get_gain(const ost_camera *camera, uint32_t *gain);

/**
 * Number of frames the camera reads per ost_camera_acquire (register 0x3c), summed on the host like WinSpec's
 * Accumulations. Supported register values are 1, 3 and 10; any other count returns OST_ERR_UNSUPPORTED naming
 * register 0x3c. With a count above 1, ost_camera_acquire stores signed 32-bit little-endian sums (WinSpec's LONG SPE
 * data type), so a frame needs 4 bytes per pixel; streaming refuses it.
 */
int ost_camera_set_accumulations(ost_camera *camera, uint32_t accumulations);

/** Accumulation count of the next acquisition; 1 until set. Host-side. */
int ost_camera_get_accumulations(const ost_camera *camera, uint32_t *accumulations);

/**
 * Set the sensor ROI and integer binning. Coordinates and sizes are sensor pixels;
 * nonzero dimensions must fit inside the sensor and be divisible by their binning
 * factors. Nothing is sent until acquisition; a selection the protocol rules do not
 * determine returns OST_ERR_UNSUPPORTED without changing the selection. A replay or
 * mock transport stores any valid selection for a replay-sized acquisition.
 */
int ost_camera_set_readout(ost_camera *camera, const ost_camera_readout *readout);

/**
 * Select 1000000 or 100000 Hz for the next acquisition. The slower rate is
 * supported only for the full unbinned frame. Nothing is sent until
 * acquisition; unsupported rates and geometry combinations change no state.
 */
int ost_camera_set_readout_rate(ost_camera *camera, uint32_t readout_rate_hz);

/** Current ROI and binning; host-side, always available and never sends anything. */
int ost_camera_get_readout(const ost_camera *camera, ost_camera_readout *readout);

/**
 * Select the full 1340-pixel sensor width with all 100 rows binned vertically.
 * The resulting frame has shape 1x1340. Behavior otherwise matches set_readout.
 */
int ost_camera_set_full_vertical_binning(ost_camera *camera);

/**
 * Acquire one binned readout frame of 16-bit samples (32-bit sums with accumulations above 1, see
 * ost_camera_set_accumulations). Its dimensions are
 * readout.width/x_binning by readout.height/y_binning. capacity must be at least
 * the frame size or OST_ERR_BUFFER_TOO_SMALL is returned before anything is sent.
 * acquired_size receives the bytes stored. Returns OST_ERR_ABORTED when ost_camera_abort interrupted
 * the read.
 *
 * On USB this runs the exposure sequence built from the current settings, after reloading the 1 MHz speed table
 * when a 100 kHz acquisition left the other table in the controller. Samples are little-endian uint16 in row-major
 * order, identical to the SPE image bytes. A short image read returns OST_ERR_IO, never a padded frame;
 * ost_camera_abort between reads sends the stop exchange and returns OST_ERR_ABORTED.
 */
int ost_camera_acquire(ost_camera *camera, void *buffer, size_t capacity, size_t *acquired_size);

typedef int (*ost_camera_frame_callback)(void *userdata, uint32_t frame_index, const void *frame, size_t frame_size);

/**
 * Counters of a streaming acquisition; nothing is hidden: every byte that was received and not delivered as part of a
 * complete frame is counted in partial_bytes_discarded.
 *
 * frames_delivered: complete 268000-byte frames passed to the callback.
 * partial_bytes_discarded: bytes of an incomplete trailing frame, including cancelled transfers; never padded.
 * bytes_received / transfers_completed / short_transfers: image IN completions seen by the host.
 * host_stalls: times every outstanding image transfer had already completed when the host came back for data, so the
 *   camera may have been held up (a slow consumer); frames_lagged: frames whose callback outlasted the previous
 *   frame period. Frames lost inside the camera are not observable from the host.
 * stopped: nonzero when the stream ended on a stop request or a callback returning OST_ERR_ABORTED.
 */
typedef struct ost_stream_stats {
    uint32_t frames_delivered;
    uint32_t frames_lagged;
    uint32_t host_stalls;
    uint32_t max_ready;
    uint32_t short_transfers;
    uint32_t max_callback_us;
    uint64_t bytes_received;
    uint64_t transfers_completed;
    uint64_t partial_bytes_discarded;
    int stopped;
} ost_stream_stats;

/**
 * Stream frames like WinSpec Focus (frame_count 0, until ost_camera_abort) or a multi-frame Acquire (frame_count N):
 * the first-frame cycle is armed once, image IN transfers stay outstanding and each complete frame is passed
 * to callback as it arrives. The stop tail is sent at the end in every case.
 * A stop request returns OST_SUCCESS with stats->stopped set; a callback may return OST_ERR_ABORTED to
 * stop the same way, any other non-success value stops and is returned. stats is filled on every return. Streaming
 * needs the libusb transport and accumulations of 1; otherwise OST_ERR_UNSUPPORTED with nothing sent.
 */
int ost_camera_stream(ost_camera *camera, uint32_t frame_count, ost_camera_frame_callback callback, void *userdata,
    ost_stream_stats *stats);

/**
 * Interrupt a running acquisition or stream between chunks; safe from another thread and never waits for the
 * running operation. A request made while no acquisition runs is discarded when the next operation starts. The stop
 * exchange is sent by the acquiring thread. A closed handle returns OST_ERR_CLOSED.
 */
int ost_camera_abort(ost_camera *camera);

#ifdef __cplusplus
}
#endif

#endif
