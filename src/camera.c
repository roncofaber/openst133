/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#include "openst133.h"
#include "camera_acquire.h"
#include "camera_modes.h"
#include "camera_settings.h"
#include "camera_stream.h"
#include "camera_startup.h"
#include "hw_guard.h"
#include "protocol.h"
#include "transport_internal.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

/* Read chunk for the generic transfer path of replay and mock transports. */
#define REPLAY_CHUNK_BYTES 0x8000u

struct ost_camera {
    /* Opened on a replay or mock transport: the generic protocol path, without startup or settings exchanges. */
    int replay;
    ost_device *device;
    ost_proto *proto;
    ost_startup_identity startup;
    ost_startup_data *startup_data;
    int shutter_closed;
    int shutter_opened;
    uint32_t gain;
    uint32_t accumulations;
    double target_celsius;
    ost_camera_target_source target_source;
    ost_sensor_geometry geometry;
    ost_camera_readout readout;
    uint32_t readout_rate_hz;
    uint32_t exposure_ms;
    int exposure_set;
    ost_settings_table table_loaded;
    atomic_int abort_requested;
    char error[256];
    pthread_mutex_t lock;
    pthread_cond_t changed;
    pthread_t owner;
    unsigned int users;
    unsigned int depth;
    int busy;
    int closing;
    int teardown_deferred;
    ost_camera *next_live;
};

static _Thread_local char open_error[256];

/*
 * Every open camera is listed here so that a call on a closed (and freed) handle is recognized without touching it.
 * Lock order: live_lock, then camera->lock.
 */
static pthread_mutex_t live_lock = PTHREAD_MUTEX_INITIALIZER;
static ost_camera *live_cameras;

static _Thread_local const ost_camera *error_camera;
static _Thread_local char error_text[256];
static const char k_closed_text[] = "the camera is closed";

const char *ost_camera_open_error(void)
{
    return open_error;
}

const char *ost_status_text(int status)
{
    switch (status) {
    case OST_SUCCESS:
        return "success";
    case OST_ERR_INVALID_ARGUMENT:
        return "invalid argument";
    case OST_ERR_NO_MEMORY:
        return "out of memory";
    case OST_ERR_IO:
        return "I/O error";
    case OST_ERR_NOT_FOUND:
        return "not found";
    case OST_ERR_UNSUPPORTED:
        return "unsupported";
    case OST_ERR_ABORTED:
        return "aborted";
    case OST_ERR_BUFFER_TOO_SMALL:
        return "buffer too small";
    case OST_ERR_TIMEOUT:
        return "timeout";
    case OST_ERR_BUSY:
        return "device is busy";
    case OST_ERR_ACCESS:
        return "permission denied";
    case OST_ERR_CLOSED:
        return "camera is closed";
    default:
        return "unknown status";
    }
}

static void set_error(ost_camera *camera, const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    (void)vsnprintf(camera->error, sizeof(camera->error), format, arguments);
    va_end(arguments);
}

static int unsupported(ost_camera *camera, const char *operation, const char *reason)
{
    set_error(camera, "%s is not supported yet: %s", operation, reason);
    return OST_ERR_UNSUPPORTED;
}

static int invalid_argument(ost_camera *camera, const char *reason)
{
    set_error(camera, "invalid argument: %s", reason);
    return OST_ERR_INVALID_ARGUMENT;
}

static int from_proto_status(int status)
{
    switch (status) {
    case OST_PROTO_OK:
        return OST_SUCCESS;
    case OST_PROTO_ERR_INVALID_ARGUMENT:
        return OST_ERR_INVALID_ARGUMENT;
    case OST_PROTO_ERR_NO_MEMORY:
        return OST_ERR_NO_MEMORY;
    case OST_PROTO_ERR_NO_DEVICE:
        return OST_ERR_NOT_FOUND;
    case OST_PROTO_ERR_UNSUPPORTED:
        return OST_ERR_UNSUPPORTED;
    case OST_PROTO_ERR_TIMEOUT:
        return OST_ERR_TIMEOUT;
    case OST_PROTO_ERR_STOPPED:
        return OST_ERR_ABORTED;
    default:
        return OST_ERR_IO;
    }
}

static int is_real_transport(const ost_transport *transport)
{
    return transport == NULL || transport == ost_libusb_transport();
}

static int replay_unsupported(ost_camera *camera, const char *operation)
{
    return unsupported(camera, operation, "replay and mock transports implement only exposure, readout and acquisition");
}

static int live_find(const ost_camera *camera)
{
    const ost_camera *live;

    for (live = live_cameras; live != NULL; live = live->next_live) {
        if (live == camera) {
            return 1;
        }
    }
    return 0;
}

static int live_remove(ost_camera *camera)
{
    ost_camera **link;

    for (link = &live_cameras; *link != NULL; link = &(*link)->next_live) {
        if (*link == camera) {
            *link = camera->next_live;
            return 1;
        }
    }
    return 0;
}

static void publish_error(const ost_camera *camera, const char *text)
{
    if (text != error_text) {
        (void)snprintf(error_text, sizeof(error_text), "%s", text);
    }
    error_camera = camera;
}

/* Locks camera->lock when the camera is still open; holding live_lock until then keeps close from freeing it. */
static int lock_live(const ost_camera *camera)
{
    ost_camera *live = (ost_camera *)camera;

    if (camera == NULL) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    pthread_mutex_lock(&live_lock);
    if (!live_find(camera)) {
        pthread_mutex_unlock(&live_lock);
        publish_error(camera, k_closed_text);
        return OST_ERR_CLOSED;
    }
    pthread_mutex_lock(&live->lock);
    pthread_mutex_unlock(&live_lock);
    return OST_SUCCESS;
}

static void unlock_camera(const ost_camera *camera)
{
    pthread_mutex_unlock(&((ost_camera *)camera)->lock);
}

static void destroy_camera(ost_camera *camera)
{
    if (camera->proto != NULL) {
        ost_proto_close(camera->proto);
    }
    if (camera->device != NULL) {
        ost_close(camera->device);
    }
    ost_startup_data_free(camera->startup_data);
    pthread_cond_destroy(&camera->changed);
    pthread_mutex_destroy(&camera->lock);
    free(camera);
}

/*
 * A 100 kHz acquisition leaves the 100 kHz speed table in the controller, and command 0x48 needs the 1 MHz table to
 * plan the next one. Rerunning the open-time startup sequence on the open handle reloads it, as reopening would.
 */
static int reload_speed_table(ost_camera *camera)
{
    ost_startup_options options;
    ost_startup_identity identity;
    int status;

    if (camera->table_loaded == OST_SETTINGS_TABLE_1_MHZ) {
        return OST_SUCCESS;
    }
    ost_startup_options_default(&options);
    options.data = camera->startup_data;
    options.setpoint_code = ost_startup_setpoint_code(camera->target_celsius);
    status = ost_startup_run(camera->device, &options, &identity, camera->error, sizeof(camera->error));
    if (status == OST_SUCCESS) {
        camera->table_loaded = OST_SETTINGS_TABLE_1_MHZ;
    }
    return status;
}

/* With camera->lock held, after a thread stopped using the camera: nonzero when that thread must destroy it. */
static int release_user(ost_camera *camera)
{
    --camera->users;
    pthread_cond_broadcast(&camera->changed);
    return camera->teardown_deferred && camera->users == 0u;
}

/*
 * Waits until no other thread runs an operation on the camera and claims it. A call from the claiming thread itself
 * (a frame callback) nests instead of waiting. Claiming clears a stale abort request; close sets it again afterwards
 * under the same lock, so a close is never lost.
 */
static int begin_operation(ost_camera *camera)
{
    int status = lock_live(camera);

    if (status != OST_SUCCESS) {
        return status;
    }
    if (camera->busy && pthread_equal(camera->owner, pthread_self())) {
        ++camera->depth;
    } else {
        ++camera->users;
        while (camera->busy && !camera->closing) {
            pthread_cond_wait(&camera->changed, &camera->lock);
        }
        if (camera->closing) {
            int destroy = release_user(camera);

            pthread_mutex_unlock(&camera->lock);
            if (destroy) {
                destroy_camera(camera);
            }
            publish_error(camera, k_closed_text);
            return OST_ERR_CLOSED;
        }
        camera->busy = 1;
        camera->owner = pthread_self();
        camera->depth = 0u;
        atomic_store(&camera->abort_requested, 0);
    }
    pthread_mutex_unlock(&camera->lock);
    camera->error[0] = '\0';
    return OST_SUCCESS;
}

static void end_operation(ost_camera *camera)
{
    int destroy = 0;

    publish_error(camera, camera->error);
    pthread_mutex_lock(&camera->lock);
    if (camera->depth > 0u) {
        --camera->depth;
    } else {
        camera->busy = 0;
        destroy = release_user(camera);
    }
    pthread_mutex_unlock(&camera->lock);
    if (destroy) {
        destroy_camera(camera);
    }
}

static void settings_of(const ost_camera *camera, ost_settings *settings)
{
    memset(settings, 0, sizeof(*settings));
    settings->exposure_ms = camera->exposure_ms;
    settings->shutter_closed = camera->shutter_closed;
    settings->shutter_opened = camera->shutter_opened;
    settings->gain = camera->gain;
    settings->accumulations = camera->accumulations;
    settings->readout_rate_hz = camera->readout_rate_hz;
    settings->readout = &camera->readout;
}

static int valid_readout(const ost_camera *camera, const ost_camera_readout *readout)
{
    if (readout->width == 0u || readout->height == 0u ||
        readout->x_binning == 0u || readout->y_binning == 0u ||
        readout->x >= camera->geometry.width || readout->y >= camera->geometry.height) {
        return 0;
    }
    if (readout->width > camera->geometry.width - readout->x ||
        readout->height > camera->geometry.height - readout->y) {
        return 0;
    }
    if (readout->width % readout->x_binning != 0u || readout->height % readout->y_binning != 0u) {
        return 0;
    }
    return 1;
}

static int has_partial_bins(const ost_camera_readout *readout)
{
    return readout->x_binning != 0u && readout->y_binning != 0u &&
        (readout->width % readout->x_binning != 0u || readout->height % readout->y_binning != 0u);
}

static void target_root(char *buffer, size_t size)
{
    const char *root = getenv("OPENST133_DATA");
#ifdef _WIN32
    const char *base = getenv("APPDATA");
#else
    const char *base = getenv("HOME");
#endif

    if (root != NULL && root[0] != '\0') {
        (void)snprintf(buffer, size, "%s", root);
#ifdef _WIN32
    } else if (base != NULL && base[0] != '\0') {
        (void)snprintf(buffer, size, "%s/openst133", base);
#endif
    } else {
#ifdef _WIN32
        (void)snprintf(buffer, size, "./openst133");
#else
        (void)snprintf(buffer, size, "%s/.local/share/openst133", base != NULL ? base : ".");
#endif
    }
}

static void target_path(const ost_device_info *info, char *buffer, size_t size)
{
    char root[1024];
    char port_path[64];

    target_root(root, sizeof(root));
    if (ost_device_port_path(info, port_path, sizeof(port_path)) == OST_SUCCESS) {
        (void)snprintf(buffer, size, "%s/state/setpoint-%04x-%04x-port-%s.txt", root,
            (unsigned int)info->vendor_id, (unsigned int)info->product_id, port_path);
        return;
    }
    (void)snprintf(buffer, size, "%s/state/setpoint-%04x-%04x-%03u-%03u.txt", root,
        (unsigned int)info->vendor_id, (unsigned int)info->product_id,
        (unsigned int)info->bus_number, (unsigned int)info->device_address);
}

static int make_directory(const char *path)
{
#ifdef _WIN32
    int status = _mkdir(path);
#else
    int status = mkdir(path, 0700);
#endif
    return status == 0 || errno == EEXIST ? OST_SUCCESS : OST_ERR_IO;
}

static int load_persisted_target(const ost_device_info *info, double *target, int *found, char *error, size_t error_size)
{
    char path[1200];
    char line[128];
    char extra;
    FILE *file;

    *found = 0;
    target_path(info, path, sizeof(path));
    file = fopen(path, "r");
    if (file == NULL) {
        if (errno == ENOENT) {
            return OST_SUCCESS;
        }
        (void)snprintf(error, error_size, "cannot read persisted target %.160s: %s", path, strerror(errno));
        return OST_ERR_IO;
    }
    if (fgets(line, sizeof(line), file) == NULL || sscanf(line, "OSTTARGET 1 %lf %c", target, &extra) != 1 ||
        ost_startup_setpoint_code(*target) < 0) {
        (void)fclose(file);
        (void)snprintf(error, error_size, "persisted target file %.160s is invalid", path);
        return OST_ERR_IO;
    }
    if (fclose(file) != 0) {
        (void)snprintf(error, error_size, "cannot close persisted target %.160s: %s", path, strerror(errno));
        return OST_ERR_IO;
    }
    *found = 1;
    return OST_SUCCESS;
}

static int persist_target(ost_camera *camera, double target)
{
    ost_device_info info;
    char root[1024];
    char directory[1200];
    char path[1400];
    FILE *file;
    int failed;
    int status = ost_get_info(camera->device, &info);

    if (status != OST_SUCCESS) {
        return status;
    }
    target_root(root, sizeof(root));
    (void)snprintf(directory, sizeof(directory), "%s/state", root);
    if (make_directory(root) != OST_SUCCESS || make_directory(directory) != OST_SUCCESS) {
        set_error(camera, "target was written, but the persistence directory %s could not be created: %s",
            directory, strerror(errno));
        return OST_ERR_IO;
    }
    target_path(&info, path, sizeof(path));
    file = fopen(path, "w");
    if (file == NULL) {
        set_error(camera, "target was written, but persisted target %s could not be opened: %s",
            path, strerror(errno));
        return OST_ERR_IO;
    }
    failed = fprintf(file, "OSTTARGET 1 %.17g\n", target) < 0;
    if (fclose(file) != 0) {
        failed = 1;
    }
    if (failed) {
        set_error(camera, "target was written, but persisted target %s could not be saved", path);
        return OST_ERR_IO;
    }
    return OST_SUCCESS;
}

void ost_camera_open_options_default(ost_camera_open_options *options)
{
    if (options != NULL) {
        memset(options, 0, sizeof(*options));
    }
}

int ost_camera_open_with_options(
    const ost_transport *transport,
    const ost_device_info *info,
    const ost_camera_open_options *open_options,
    ost_camera **camera)
{
    ost_camera *opened;
    ost_startup_data *startup_data = NULL;
    ost_camera_target_source selected_source = OST_CAMERA_TARGET_SOURCE_NONE;
    double selected_target = 0.0;
    int selected_code = -1;
    int real = is_real_transport(transport);
    int status;

    if (info == NULL || camera == NULL) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    *camera = NULL;
    open_error[0] = '\0';
    if (real) {
        status = ost_hw_guard_check();
        if (status != OST_SUCCESS) {
            (void)snprintf(open_error, sizeof(open_error), "%s", ost_hw_guard_error());
            return status;
        }
    }
    if (open_options != NULL && open_options->target_set) {
        if (!real) {
            (void)snprintf(open_error, sizeof(open_error),
                "an explicit startup target applies only to a camera on the USB transport");
            return OST_ERR_INVALID_ARGUMENT;
        }
        selected_code = ost_startup_setpoint_code(open_options->target_celsius);
        if (selected_code < 0) {
            (void)snprintf(open_error, sizeof(open_error),
                "startup target %g C is outside the selected range of -152 C to -25 C", open_options->target_celsius);
            return OST_ERR_INVALID_ARGUMENT;
        }
        selected_target = open_options->target_celsius;
        selected_source = OST_CAMERA_TARGET_SOURCE_OPTION;
    }
    opened = calloc(1u, sizeof(*opened));
    if (opened == NULL) {
        return OST_ERR_NO_MEMORY;
    }
    opened->replay = !real;
    if (real && selected_source == OST_CAMERA_TARGET_SOURCE_NONE) {
        int found = 0;

        status = load_persisted_target(info, &selected_target, &found, open_error, sizeof(open_error));
        if (status != OST_SUCCESS) {
            free(opened);
            return status;
        }
        if (found) {
            selected_code = ost_startup_setpoint_code(selected_target);
            selected_source = OST_CAMERA_TARGET_SOURCE_PERSISTED;
        }
    }
    if (real) {
        status = ost_startup_data_load(NULL, &startup_data, open_error, sizeof(open_error));
        if (status != OST_SUCCESS) {
            free(opened);
            return status;
        }
    }
    opened->geometry.width = OST_CAMERA_DEFAULT_WIDTH;
    opened->geometry.height = OST_CAMERA_DEFAULT_HEIGHT;
    opened->readout.width = OST_CAMERA_DEFAULT_WIDTH;
    opened->readout.height = OST_CAMERA_DEFAULT_HEIGHT;
    opened->readout.x_binning = 1u;
    opened->readout.y_binning = 1u;
    opened->readout_rate_hz = OST_CAMERA_READOUT_RATE_1_MHZ;
    atomic_init(&opened->abort_requested, 0);
    if (real) {
        status = ost_open(transport, info, &opened->device);
        if (status != OST_SUCCESS && open_error[0] == '\0') {
            (void)snprintf(open_error, sizeof(open_error), "%s", ost_open_error());
        }
    } else {
        status = from_proto_status(ost_proto_open(transport, info, NULL, &opened->proto));
    }
    if (status == OST_SUCCESS && real) {
        ost_startup_options options;

        ost_startup_options_default(&options);
        options.data = startup_data;
        options.setpoint_code = selected_code;
        status = ost_startup_run(opened->device, &options, &opened->startup, open_error, sizeof(open_error));
        if (status == OST_SUCCESS) {
            opened->geometry.width = opened->startup.width;
            opened->geometry.height = opened->startup.height;
            opened->geometry.reported = 1;
            opened->readout.width = opened->startup.width;
            opened->readout.height = opened->startup.height;
            if (selected_source == OST_CAMERA_TARGET_SOURCE_NONE) {
                if (opened->startup.target_known) {
                    selected_target = (double)opened->startup.target_celsius;
                    selected_source = OST_CAMERA_TARGET_SOURCE_CAMERA_DEFAULT;
                } else {
                    selected_target = OST_STARTUP_DEFAULT_TARGET_CELSIUS;
                    selected_source = OST_CAMERA_TARGET_SOURCE_STARTUP_DEFAULT;
                }
            }
            opened->target_celsius = selected_target;
            opened->target_source = selected_source;
        } else {
            ost_close(opened->device);
            opened->device = NULL;
        }
    }
    if (status == OST_SUCCESS) {
        if (pthread_mutex_init(&opened->lock, NULL) != 0) {
            status = OST_ERR_NO_MEMORY;
        } else if (pthread_cond_init(&opened->changed, NULL) != 0) {
            pthread_mutex_destroy(&opened->lock);
            status = OST_ERR_NO_MEMORY;
        }
        if (status != OST_SUCCESS) {
            if (opened->proto != NULL) {
                ost_proto_close(opened->proto);
            }
            if (opened->device != NULL) {
                ost_close(opened->device);
            }
        }
    }
    if (status != OST_SUCCESS) {
        ost_startup_data_free(startup_data);
        free(opened);
        return status;
    }
    opened->startup_data = startup_data;
    pthread_mutex_lock(&live_lock);
    opened->next_live = live_cameras;
    live_cameras = opened;
    pthread_mutex_unlock(&live_lock);
    *camera = opened;
    return OST_SUCCESS;
}

int ost_camera_open(const ost_transport *transport, const ost_device_info *info, ost_camera **camera)
{
    return ost_camera_open_with_options(transport, info, NULL, camera);
}

void ost_camera_close(ost_camera *camera)
{
    if (camera == NULL) {
        return;
    }
    pthread_mutex_lock(&live_lock);
    if (!live_remove(camera)) {
        pthread_mutex_unlock(&live_lock);
        return;
    }
    pthread_mutex_lock(&camera->lock);
    pthread_mutex_unlock(&live_lock);
    camera->closing = 1;
    atomic_store(&camera->abort_requested, 1);
    pthread_cond_broadcast(&camera->changed);
    if (camera->busy && pthread_equal(camera->owner, pthread_self())) {
        /* Called from a frame callback: the operation running below it destroys the camera when it ends. */
        camera->teardown_deferred = 1;
        pthread_mutex_unlock(&camera->lock);
        return;
    }
    while (camera->users > 0u) {
        pthread_cond_wait(&camera->changed, &camera->lock);
    }
    pthread_mutex_unlock(&camera->lock);
    destroy_camera(camera);
}

static int query_support_locked(const ost_camera *camera, ost_camera_operation operation)
{
    switch (operation) {
    case OST_CAMERA_OP_GET_TEMPERATURE:
    case OST_CAMERA_OP_SET_TEMPERATURE:
        return camera->replay ? OST_ERR_UNSUPPORTED : OST_SUCCESS;
    case OST_CAMERA_OP_SET_EXPOSURE:
    case OST_CAMERA_OP_SET_READOUT:
    case OST_CAMERA_OP_ACQUIRE:
    case OST_CAMERA_OP_ABORT:
        return OST_SUCCESS;
    default:
        return OST_ERR_INVALID_ARGUMENT;
    }
}

int ost_camera_query_support(const ost_camera *camera, ost_camera_operation operation)
{
    int status = lock_live(camera);

    if (status == OST_SUCCESS) {
        status = query_support_locked(camera, operation);
        unlock_camera(camera);
    }
    return status;
}

int ost_camera_get_target_source(const ost_camera *camera, ost_camera_target_source *source)
{
    int status = source == NULL ? OST_ERR_INVALID_ARGUMENT : lock_live(camera);

    if (status == OST_SUCCESS) {
        *source = camera->target_source;
        unlock_camera(camera);
    }
    return status;
}

const char *ost_camera_last_error(const ost_camera *camera)
{
    int live;

    if (camera == NULL) {
        return "no camera";
    }
    pthread_mutex_lock(&live_lock);
    live = live_find(camera);
    pthread_mutex_unlock(&live_lock);
    if (!live) {
        return k_closed_text;
    }
    return error_camera == camera ? error_text : "";
}

int ost_camera_get_geometry(const ost_camera *camera, ost_sensor_geometry *geometry)
{
    int status = geometry == NULL ? OST_ERR_INVALID_ARGUMENT : lock_live(camera);

    if (status == OST_SUCCESS) {
        *geometry = camera->geometry;
        unlock_camera(camera);
    }
    return status;
}

int ost_camera_get_identity(const ost_camera *camera, ost_camera_identity *identity)
{
    int status = identity == NULL ? OST_ERR_INVALID_ARGUMENT : lock_live(camera);

    if (status != OST_SUCCESS) {
        return status;
    }
    memset(identity, 0, sizeof(*identity));
    if (!camera->replay) {
        identity->controller_type = camera->startup.controller_type;
        identity->detector_id = camera->startup.detector_id;
        (void)snprintf(identity->detector_name, sizeof(identity->detector_name), "%s",
            camera->startup.detector_name);
        identity->width = camera->startup.width;
        identity->height = camera->startup.height;
        identity->target_known =
            camera->startup.target_known || camera->target_source != OST_CAMERA_TARGET_SOURCE_NONE;
        identity->target_celsius = camera->target_celsius;
    } else {
        status = OST_ERR_UNSUPPORTED;
    }
    unlock_camera(camera);
    return status;
}

static int get_temperature_locked(ost_camera *camera, double *celsius)
{
    int status;

    if (camera == NULL) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    if (celsius == NULL) {
        return invalid_argument(camera, "the temperature output is NULL");
    }
    if (camera->replay) {
        return replay_unsupported(camera, "temperature read");
    }
    status = ost_startup_read_temperature(
        camera->device, &camera->startup, 0, celsius, camera->error, sizeof(camera->error));
    return status;
}

static int set_temperature_locked(ost_camera *camera, double celsius)
{
    int status;

    if (camera == NULL) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    if (camera->replay) {
        return replay_unsupported(camera, "temperature setpoint");
    }
    status = ost_startup_write_setpoint(
        camera->device, &camera->startup, 0, celsius, camera->error, sizeof(camera->error));
    if (status == OST_SUCCESS) {
        pthread_mutex_lock(&camera->lock);
        camera->target_celsius = celsius;
        pthread_mutex_unlock(&camera->lock);
        status = persist_target(camera, celsius);
    }
    return status;
}

static int set_exposure_locked(ost_camera *camera, uint32_t exposure_ms)
{
    if (camera == NULL) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    if (exposure_ms == 0u && camera->replay) {
        return invalid_argument(camera, "an exposure of 0 ms needs a camera on the USB transport");
    }
    if (!camera->replay) {
        int status = ost_settings_encode_exposure(exposure_ms, NULL, NULL, camera->error, sizeof(camera->error));

        if (status == OST_SUCCESS) {
            pthread_mutex_lock(&camera->lock);
            camera->exposure_ms = exposure_ms;
            camera->exposure_set = 1;
            pthread_mutex_unlock(&camera->lock);
        }
        return status;
    }
    pthread_mutex_lock(&camera->lock);
    camera->exposure_ms = exposure_ms;
    pthread_mutex_unlock(&camera->lock);
    return OST_SUCCESS;
}

static int set_shutter_locked(ost_camera *camera, ost_shutter_state state)
{
    if (camera == NULL) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    if (state != OST_SHUTTER_OPEN && state != OST_SHUTTER_CLOSED && state != OST_SHUTTER_DISABLED_OPENED &&
        state != OST_SHUTTER_PRE_OPEN) {
        return invalid_argument(camera, "the shutter state is not Normal, Disabled Closed, Disabled Opened or Pre Open");
    }
    if (camera->replay) {
        return replay_unsupported(camera, "shutter selection");
    }
    if (state == OST_SHUTTER_DISABLED_OPENED || state == OST_SHUTTER_PRE_OPEN) {
        ost_settings settings;
        ost_settings_plan plan;
        int status;

        settings_of(camera, &settings);
        settings.shutter_closed = 0;
        settings.shutter_opened = state == OST_SHUTTER_DISABLED_OPENED;
        settings.pre_open = state == OST_SHUTTER_PRE_OPEN;
        status = ost_settings_plan_make(&settings, OST_SETTINGS_TABLE_1_MHZ, &plan, camera->error,
            sizeof(camera->error));
        if (status != OST_SUCCESS) {
            return status;
        }
    }
    pthread_mutex_lock(&camera->lock);
    camera->shutter_closed = state == OST_SHUTTER_CLOSED;
    camera->shutter_opened = state == OST_SHUTTER_DISABLED_OPENED;
    pthread_mutex_unlock(&camera->lock);
    return OST_SUCCESS;
}

static int set_gain_locked(ost_camera *camera, uint32_t gain)
{
    ost_settings settings;
    ost_settings_plan plan;
    int status;

    if (camera == NULL) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    if (gain == 0u) {
        return invalid_argument(camera, "controller gain 0 does not exist; WinSpec offers 1, 2 and 3");
    }
    if (camera->replay) {
        return replay_unsupported(camera, "controller gain selection");
    }
    settings_of(camera, &settings);
    settings.gain = gain;
    status = ost_settings_plan_make(&settings, OST_SETTINGS_TABLE_1_MHZ, &plan, camera->error, sizeof(camera->error));
    if (status == OST_SUCCESS) {
        pthread_mutex_lock(&camera->lock);
        camera->gain = gain;
        pthread_mutex_unlock(&camera->lock);
    }
    return status;
}

static int set_accumulations_locked(ost_camera *camera, uint32_t accumulations)
{
    ost_settings settings;
    ost_settings_plan plan;
    int status;

    if (camera == NULL) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    if (accumulations == 0u) {
        return invalid_argument(camera, "the accumulation count is 0");
    }
    if (camera->replay) {
        return replay_unsupported(camera, "accumulations");
    }
    settings_of(camera, &settings);
    settings.accumulations = accumulations;
    status = ost_settings_plan_make(&settings, OST_SETTINGS_TABLE_1_MHZ, &plan, camera->error, sizeof(camera->error));
    if (status == OST_SUCCESS) {
        pthread_mutex_lock(&camera->lock);
        camera->accumulations = accumulations;
        pthread_mutex_unlock(&camera->lock);
    }
    return status;
}

int ost_camera_get_gain(const ost_camera *camera, uint32_t *gain)
{
    int status = gain == NULL ? OST_ERR_INVALID_ARGUMENT : lock_live(camera);

    if (status == OST_SUCCESS) {
        *gain = camera->gain != 0u ? camera->gain : 2u;
        unlock_camera(camera);
    }
    return status;
}

int ost_camera_get_accumulations(const ost_camera *camera, uint32_t *accumulations)
{
    int status = accumulations == NULL ? OST_ERR_INVALID_ARGUMENT : lock_live(camera);

    if (status == OST_SUCCESS) {
        *accumulations = camera->accumulations != 0u ? camera->accumulations : 1u;
        unlock_camera(camera);
    }
    return status;
}

int ost_camera_get_shutter(const ost_camera *camera, ost_shutter_state *state)
{
    int status = state == NULL ? OST_ERR_INVALID_ARGUMENT : lock_live(camera);

    if (status == OST_SUCCESS) {
        *state = camera->shutter_closed ? OST_SHUTTER_CLOSED
            : camera->shutter_opened ? OST_SHUTTER_DISABLED_OPENED : OST_SHUTTER_OPEN;
        unlock_camera(camera);
    }
    return status;
}

int ost_camera_get_exposure(const ost_camera *camera, uint32_t *exposure_ms)
{
    int status = exposure_ms == NULL ? OST_ERR_INVALID_ARGUMENT : lock_live(camera);

    if (status == OST_SUCCESS) {
        *exposure_ms = camera->exposure_ms;
        unlock_camera(camera);
    }
    return status;
}

int ost_camera_get_effective_exposure(const ost_camera *camera, uint32_t *exposure_ms)
{
    unsigned int scale = 0u;
    uint32_t stored;
    int status = exposure_ms == NULL ? OST_ERR_INVALID_ARGUMENT : lock_live(camera);

    if (status != OST_SUCCESS) {
        return status;
    }
    stored = camera->exposure_ms;
    *exposure_ms = stored;
    if (!camera->replay) {
        status = ost_settings_encode_exposure(stored, NULL, &scale, NULL, 0u);
        if (status == OST_SUCCESS) {
            *exposure_ms = (stored >> scale) << scale;
        }
    }
    unlock_camera(camera);
    return status;
}

static int set_readout_locked(ost_camera *camera, const ost_camera_readout *readout)
{
    if (camera == NULL) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    if (readout == NULL) {
        return invalid_argument(camera, "the readout is NULL");
    }
    if (has_partial_bins(readout)) {
        set_error(camera, "ROI and binning must fit the sensor with positive extents and bins");
        return OST_ERR_INVALID_ARGUMENT;
    }
    if (!valid_readout(camera, readout)) {
        set_error(camera, "ROI and binning must fit the sensor with positive extents and bins");
        return OST_ERR_INVALID_ARGUMENT;
    }
    if (!camera->replay) {
        ost_settings settings;
        ost_settings_plan plan;
        int status;

        settings_of(camera, &settings);
        settings.readout = readout;
        status = ost_settings_plan_make(&settings, OST_SETTINGS_TABLE_1_MHZ, &plan, camera->error,
            sizeof(camera->error));
        if (status != OST_SUCCESS) {
            return status;
        }
    }
    pthread_mutex_lock(&camera->lock);
    camera->readout = *readout;
    pthread_mutex_unlock(&camera->lock);
    return OST_SUCCESS;
}

static int set_readout_rate_locked(ost_camera *camera, uint32_t readout_rate_hz)
{
    ost_settings settings;
    ost_settings_plan plan;
    int status;

    if (camera == NULL) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    if (readout_rate_hz == 0u) {
        return invalid_argument(camera, "the readout rate is 0 Hz");
    }
    if (camera->replay) {
        return replay_unsupported(camera, "readout rate");
    }
    settings_of(camera, &settings);
    settings.readout_rate_hz = readout_rate_hz;
    status = ost_settings_plan_make(&settings, OST_SETTINGS_TABLE_1_MHZ, &plan, camera->error,
        sizeof(camera->error));
    if (status == OST_SUCCESS) {
        camera->readout_rate_hz = readout_rate_hz;
    }
    return status;
}

int ost_camera_get_readout(const ost_camera *camera, ost_camera_readout *readout)
{
    int status = readout == NULL ? OST_ERR_INVALID_ARGUMENT : lock_live(camera);

    if (status == OST_SUCCESS) {
        *readout = camera->readout;
        unlock_camera(camera);
    }
    return status;
}

static int set_full_vertical_binning_locked(ost_camera *camera)
{
    ost_camera_readout readout;

    if (camera == NULL) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    readout.x = 0u;
    readout.y = 0u;
    readout.width = camera->geometry.width;
    readout.height = camera->geometry.height;
    readout.x_binning = 1u;
    readout.y_binning = camera->geometry.height;
    return set_readout_locked(camera, &readout);
}

static int acquire_locked(ost_camera *camera, void *buffer, size_t capacity, size_t *acquired_size)
{
    size_t total;
    size_t done = 0u;
    ost_proto_transfer_params params;
    unsigned char *bytes = buffer;
    int status;

    if (camera == NULL) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    if (buffer == NULL || acquired_size == NULL) {
        return invalid_argument(camera, "the frame buffer or the size output is NULL");
    }
    *acquired_size = 0u;
    if (!camera->replay) {
        ost_acquire_options options;
        ost_settings settings;
        ost_settings_plan plan;
        int result;

        if (!camera->exposure_set) {
            return unsupported(camera, "acquisition", "set an exposure first");
        }
        settings_of(camera, &settings);
        result = reload_speed_table(camera);
        if (result != OST_SUCCESS) {
            return result;
        }
        result = ost_settings_plan_make(&settings, camera->table_loaded, &plan, camera->error,
            sizeof(camera->error));
        if (result != OST_SUCCESS) {
            return result;
        }
        memset(&options, 0, sizeof(options));
        options.exposure_ms = camera->exposure_ms;
        options.data = camera->startup_data;
        options.plan = &plan;
        options.abort_requested = &camera->abort_requested;
        result = ost_acquire_run(camera->device, &options, buffer, capacity, acquired_size, camera->error,
            sizeof(camera->error));
        if (result == OST_SUCCESS) {
            camera->table_loaded = plan.table_after;
        } else if (plan.insert_rate_table && result != OST_ERR_BUFFER_TOO_SMALL) {
            camera->table_loaded = OST_SETTINGS_TABLE_UNKNOWN;
        }
        return result;
    }
    total = (size_t)(camera->readout.width / camera->readout.x_binning)
        * (camera->readout.height / camera->readout.y_binning) * OST_CAMERA_BYTES_PER_PIXEL;
    if (capacity < total) {
        set_error(camera, "buffer holds %zu bytes but a frame needs %zu", capacity, total);
        return OST_ERR_BUFFER_TOO_SMALL;
    }
    params.configured_size = (uint32_t)total;
    params.chunk_size = REPLAY_CHUNK_BYTES;
    params.total_size = (uint32_t)total;
    status = from_proto_status(ost_proto_setup_transfer(camera->proto, &params));
    while (status == OST_SUCCESS && done < total) {
        size_t want = total - done < REPLAY_CHUNK_BYTES ? total - done : REPLAY_CHUNK_BYTES;
        size_t received = 0u;

        if (atomic_load(&camera->abort_requested) != 0) {
            status = OST_ERR_ABORTED;
            break;
        }
        status = from_proto_status(ost_proto_read_image(camera->proto, bytes + done, want, want, &received));
        done += received;
    }
    *acquired_size = done;
    if (status != OST_SUCCESS) {
        set_error(camera, "acquisition failed after %zu of %zu bytes: %s", done, total, ost_status_text(status));
    }
    return status;
}

static int stream_locked(ost_camera *camera, uint32_t frame_count, ost_camera_frame_callback callback, void *userdata,
    ost_stream_stats *stats)
{
    ost_acquire_options options;

    if (stats != NULL) {
        memset(stats, 0, sizeof(*stats));
    }
    if (camera == NULL) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    if (callback == NULL || stats == NULL) {
        return invalid_argument(camera, "the callback or the statistics output is NULL");
    }
    if (camera->replay) {
        return replay_unsupported(camera, "streaming");
    }
    {
        ost_settings settings;
        ost_settings_plan plan;
        int result;

        if (!camera->exposure_set) {
            return unsupported(camera, "streaming", "set an exposure first");
        }
        if (camera->accumulations > 1u) {
            return unsupported(camera, "streaming with accumulations",
                "a stream programs register 0x3c itself; set accumulations to 1");
        }
        settings_of(camera, &settings);
        result = reload_speed_table(camera);
        if (result != OST_SUCCESS) {
            return result;
        }
        result = ost_settings_plan_make(&settings, camera->table_loaded, &plan, camera->error, sizeof(camera->error));
        if (result != OST_SUCCESS) {
            return result;
        }
        memset(&options, 0, sizeof(options));
        options.exposure_ms = camera->exposure_ms;
        options.data = camera->startup_data;
        options.plan = &plan;
        options.abort_requested = &camera->abort_requested;
        result = ost_camera_stream_run(camera->device, &options, NULL, frame_count, callback, userdata, stats,
            camera->error, sizeof(camera->error));
        if (result == OST_SUCCESS) {
            camera->table_loaded = plan.table_after;
        } else if (plan.insert_rate_table && result != OST_ERR_BUFFER_TOO_SMALL) {
            camera->table_loaded = OST_SETTINGS_TABLE_UNKNOWN;
        }
        return result;
    }
}

int ost_camera_abort(ost_camera *camera)
{
    int status = OST_SUCCESS;

    if (camera == NULL) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    pthread_mutex_lock(&live_lock);
    if (!live_find(camera)) {
        status = OST_ERR_CLOSED;
    } else {
        atomic_store(&camera->abort_requested, 1);
    }
    pthread_mutex_unlock(&live_lock);
    if (status == OST_ERR_CLOSED) {
        publish_error(camera, k_closed_text);
    }
    return status;
}

/* Public operations: each runs its body while it holds the camera (begin_operation). */

int ost_camera_get_temperature(ost_camera *camera, double *celsius)
{
    int status = begin_operation(camera);

    if (status == OST_SUCCESS) {
        status = get_temperature_locked(camera, celsius);
        end_operation(camera);
    }
    return status;
}

int ost_camera_set_temperature(ost_camera *camera, double celsius)
{
    int status = begin_operation(camera);

    if (status == OST_SUCCESS) {
        status = set_temperature_locked(camera, celsius);
        end_operation(camera);
    }
    return status;
}

int ost_camera_set_exposure(ost_camera *camera, uint32_t exposure_ms)
{
    int status = begin_operation(camera);

    if (status == OST_SUCCESS) {
        status = set_exposure_locked(camera, exposure_ms);
        end_operation(camera);
    }
    return status;
}

int ost_camera_set_shutter(ost_camera *camera, ost_shutter_state state)
{
    int status = begin_operation(camera);

    if (status == OST_SUCCESS) {
        status = set_shutter_locked(camera, state);
        end_operation(camera);
    }
    return status;
}

int ost_camera_set_gain(ost_camera *camera, uint32_t gain)
{
    int status = begin_operation(camera);

    if (status == OST_SUCCESS) {
        status = set_gain_locked(camera, gain);
        end_operation(camera);
    }
    return status;
}

int ost_camera_set_accumulations(ost_camera *camera, uint32_t accumulations)
{
    int status = begin_operation(camera);

    if (status == OST_SUCCESS) {
        status = set_accumulations_locked(camera, accumulations);
        end_operation(camera);
    }
    return status;
}

int ost_camera_set_readout(ost_camera *camera, const ost_camera_readout *readout)
{
    int status = begin_operation(camera);

    if (status == OST_SUCCESS) {
        status = set_readout_locked(camera, readout);
        end_operation(camera);
    }
    return status;
}

int ost_camera_set_readout_rate(ost_camera *camera, uint32_t readout_rate_hz)
{
    int status = begin_operation(camera);

    if (status == OST_SUCCESS) {
        status = set_readout_rate_locked(camera, readout_rate_hz);
        end_operation(camera);
    }
    return status;
}

int ost_camera_set_full_vertical_binning(ost_camera *camera)
{
    int status = begin_operation(camera);

    if (status == OST_SUCCESS) {
        status = set_full_vertical_binning_locked(camera);
        end_operation(camera);
    }
    return status;
}

int ost_camera_acquire(ost_camera *camera, void *buffer, size_t capacity, size_t *acquired_size)
{
    int status;

    if (acquired_size != NULL) {
        *acquired_size = 0u;
    }
    status = begin_operation(camera);
    if (status == OST_SUCCESS) {
        status = acquire_locked(camera, buffer, capacity, acquired_size);
        end_operation(camera);
    }
    return status;
}

int ost_camera_stream(ost_camera *camera, uint32_t frame_count, ost_camera_frame_callback callback, void *userdata,
    ost_stream_stats *stats)
{
    int status;

    if (stats != NULL) {
        memset(stats, 0, sizeof(*stats));
    }
    status = begin_operation(camera);
    if (status == OST_SUCCESS) {
        status = stream_locked(camera, frame_count, callback, userdata, stats);
        end_operation(camera);
    }
    return status;
}
