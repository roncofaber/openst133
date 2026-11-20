/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

/*
 * Threading contract of the camera handle (include/openst133.h) on a simulated transport: concurrent acquisitions
 * serialize, close during a running acquisition aborts it and waits for it, waiting callers and calls on the closed
 * handle return OST_ERR_CLOSED, and close from the acquiring thread defers the release. Meant to run under
 * ThreadSanitizer or AddressSanitizer as well as plainly; never touches hardware.
 */

#define _POSIX_C_SOURCE 200809L

#include "openst133.h"
#include "test_check.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FRAME_BYTES (OST_CAMERA_DEFAULT_WIDTH * OST_CAMERA_DEFAULT_HEIGHT * OST_CAMERA_BYTES_PER_PIXEL)
#define ACQUISITIONS_PER_THREAD 12u
#define SETTER_ROUNDS 200u

static const ost_device_info camera_info = {OST_USB_VENDOR_ID, OST_USB_PRODUCT_ID, 2u, 7u, 0u, {0u}};

typedef enum phase {
    PHASE_IDLE = 0,
    PHASE_SETUP,
    PHASE_IMAGE
} phase;

typedef enum stall_mode {
    STALL_NONE = 0,
    /* Stall in mid-frame until another thread has started closing the camera. */
    STALL_UNTIL_CLOSING,
    /* Close the camera from inside the image read, on the acquiring thread. */
    STALL_CLOSE_HERE
} stall_mode;

/*
 * The simulated device. Its protocol state is deliberately plain (not atomic): it is touched only from inside a
 * transfer, so any transfer that is not serialized by the camera shows up as a data race under ThreadSanitizer and
 * as an overlap or protocol violation here.
 */
typedef struct sim {
    atomic_int inside;
    atomic_int overlaps;
    atomic_int opens;
    atomic_int closes;
    atomic_int after_close;
    atomic_int stalled;
    phase state;
    size_t offset;
    unsigned int violations;
    unsigned int frames_started;
    unsigned int image_reads;
    stall_mode stall;
    ost_camera *camera;
} sim;

static void sleep_ms(unsigned int ms)
{
    struct timespec t;

    t.tv_sec = (time_t)(ms / 1000u);
    t.tv_nsec = (long)(ms % 1000u) * 1000000L;
    (void)nanosleep(&t, NULL);
}

static unsigned char pattern(size_t offset)
{
    return (unsigned char)((offset * 7u + (offset >> 8u)) & 0xffu);
}

static void enter(sim *s)
{
    if (atomic_fetch_add(&s->inside, 1) != 0) {
        atomic_fetch_add(&s->overlaps, 1);
    }
    if (atomic_load(&s->closes) != 0) {
        atomic_fetch_add(&s->after_close, 1);
    }
}

static void leave(sim *s)
{
    atomic_fetch_sub(&s->inside, 1);
}

static int sim_list(void *userdata, ost_device_info **devices, size_t *count)
{
    (void)userdata;
    *devices = NULL;
    *count = 0u;
    return OST_SUCCESS;
}

static void sim_free(void *userdata, ost_device_info *devices)
{
    (void)userdata;
    free(devices);
}

static int sim_open(void *userdata, const ost_device_info *info, ost_transport_handle **handle)
{
    sim *s = userdata;

    (void)info;
    atomic_fetch_add(&s->opens, 1);
    *handle = (ost_transport_handle *)s;
    return OST_SUCCESS;
}

static void sim_close(void *userdata, ost_transport_handle *handle)
{
    sim *s = userdata;

    (void)handle;
    TEST_CHECK(atomic_load(&s->inside) == 0);
    atomic_fetch_add(&s->closes, 1);
}

static int sim_control(void *userdata, ost_transport_handle *handle, uint8_t request_type, uint8_t request,
    uint16_t value, uint16_t index, unsigned char *data, uint16_t length, unsigned int timeout_ms, int *transferred)
{
    sim *s = userdata;

    (void)handle;
    (void)request_type;
    (void)value;
    (void)index;
    (void)data;
    (void)timeout_ms;
    enter(s);
    if (request == 0xf2u && s->state != PHASE_SETUP) {
        ++s->violations;
    }
    *transferred = length;
    leave(s);
    return OST_SUCCESS;
}

static void stall(sim *s)
{
    uint32_t exposure = 0u;

    if (s->stall == STALL_UNTIL_CLOSING) {
        atomic_store(&s->stalled, 1);
        while (ost_camera_get_exposure(s->camera, &exposure) != OST_ERR_CLOSED) {
            sleep_ms(1u);
        }
        /* Close sets the abort request right after it unlisted the camera. */
        sleep_ms(20u);
    } else if (s->stall == STALL_CLOSE_HERE) {
        atomic_store(&s->stalled, 1);
        ost_camera_close(s->camera);
        TEST_CHECK(atomic_load(&s->closes) == 0);
        TEST_CHECK(ost_camera_get_exposure(s->camera, &exposure) == OST_ERR_CLOSED);
    }
    s->stall = STALL_NONE;
}

static int sim_bulk(void *userdata, ost_transport_handle *handle, unsigned char endpoint, unsigned char *data,
    int length, unsigned int timeout_ms, int *transferred)
{
    sim *s = userdata;
    int i;

    (void)handle;
    (void)timeout_ms;
    enter(s);
    if ((endpoint & 0x80u) == 0u) {
        unsigned int command = length >= 3 ? (unsigned int)(data[1] | (data[2] << 8)) : 0u;

        if (command == 0x14u && length == 6 && data[4] == 0u && data[5] == 1u) {
            if (s->state != PHASE_IDLE) {
                ++s->violations;
            }
            s->state = PHASE_SETUP;
            s->offset = 0u;
            ++s->frames_started;
        } else if (command == 0x16u) {
            if (s->state != PHASE_SETUP) {
                ++s->violations;
            }
            s->state = PHASE_IMAGE;
        } else if (s->state != PHASE_SETUP) {
            ++s->violations;
        }
    } else {
        if (s->state != PHASE_IMAGE || s->offset + (size_t)length > FRAME_BYTES) {
            ++s->violations;
        } else {
            for (i = 0; i < length; ++i) {
                data[i] = pattern(s->offset + (size_t)i);
            }
            s->offset += (size_t)length;
            if (s->offset == FRAME_BYTES) {
                s->state = PHASE_IDLE;
            }
        }
        ++s->image_reads;
        if (s->stall != STALL_NONE && s->image_reads % 9u == 3u) {
            leave(s);
            stall(s);
            enter(s);
        }
    }
    *transferred = length;
    leave(s);
    return OST_SUCCESS;
}

static ost_transport sim_transport(sim *s)
{
    ost_transport transport;

    memset(&transport, 0, sizeof(transport));
    transport.userdata = s;
    transport.list_devices = sim_list;
    transport.free_devices = sim_free;
    transport.open = sim_open;
    transport.close = sim_close;
    transport.control_transfer = sim_control;
    transport.bulk_transfer = sim_bulk;
    return transport;
}

static void sim_start(sim *s, ost_transport *transport)
{
    memset(s, 0, sizeof(*s));
    atomic_init(&s->inside, 0);
    atomic_init(&s->overlaps, 0);
    atomic_init(&s->opens, 0);
    atomic_init(&s->closes, 0);
    atomic_init(&s->after_close, 0);
    atomic_init(&s->stalled, 0);
    *transport = sim_transport(s);
    TEST_CHECK(ost_camera_open(transport, &camera_info, &s->camera) == OST_SUCCESS);
    TEST_CHECK(atomic_load(&s->opens) == 1);
}

static int whole_frame(const unsigned char *frame, size_t size)
{
    size_t i;

    if (size != FRAME_BYTES) {
        return 0;
    }
    for (i = 0u; i < size; ++i) {
        if (frame[i] != pattern(i)) {
            return 0;
        }
    }
    return 1;
}

/* ---- two threads acquiring, a third changing settings ------------------------------------------------------- */

typedef struct acquirer {
    ost_camera *camera;
    unsigned int good;
    int status;
} acquirer;

static void *acquire_loop(void *argument)
{
    acquirer *a = argument;
    unsigned char *frame = malloc(FRAME_BYTES);
    unsigned int i;

    TEST_CHECK(frame != NULL);
    a->status = OST_SUCCESS;
    for (i = 0u; i < ACQUISITIONS_PER_THREAD; ++i) {
        size_t acquired = 0u;
        int status = ost_camera_acquire(a->camera, frame, FRAME_BYTES, &acquired);

        if (status != OST_SUCCESS) {
            a->status = status;
            break;
        }
        if (whole_frame(frame, acquired)) {
            ++a->good;
        }
        if (ost_camera_last_error(a->camera)[0] != '\0') {
            a->status = OST_ERR_IO;
            break;
        }
    }
    free(frame);
    return NULL;
}

typedef struct setter {
    ost_camera *camera;
    unsigned int failures;
} setter;

static void *settings_loop(void *argument)
{
    setter *t = argument;
    unsigned int i;

    for (i = 0u; i < SETTER_ROUNDS; ++i) {
        uint32_t exposure = 0u;
        ost_camera_readout readout;
        ost_sensor_geometry geometry;

        if (ost_camera_set_exposure(t->camera, 10u + i) != OST_SUCCESS ||
            ost_camera_get_exposure(t->camera, &exposure) != OST_SUCCESS || exposure == 0u ||
            ost_camera_get_readout(t->camera, &readout) != OST_SUCCESS || readout.width != OST_CAMERA_DEFAULT_WIDTH ||
            ost_camera_get_geometry(t->camera, &geometry) != OST_SUCCESS ||
            ost_camera_query_support(t->camera, OST_CAMERA_OP_ACQUIRE) != OST_SUCCESS) {
            ++t->failures;
        }
        if (ost_camera_set_exposure(t->camera, 0u) != OST_ERR_INVALID_ARGUMENT) {
            ++t->failures;
        }
        if (ost_camera_set_temperature(t->camera, -50.0) != OST_ERR_UNSUPPORTED ||
            strstr(ost_camera_last_error(t->camera), "temperature setpoint") == NULL) {
            ++t->failures;
        }
    }
    return NULL;
}

static void test_concurrent_acquisitions(void)
{
    sim s;
    ost_transport transport;
    pthread_t threads[3];
    acquirer a[2];
    setter t;
    int i;

    sim_start(&s, &transport);
    memset(a, 0, sizeof(a));
    memset(&t, 0, sizeof(t));
    a[0].camera = a[1].camera = t.camera = s.camera;
    TEST_CHECK(pthread_create(&threads[0], NULL, acquire_loop, &a[0]) == 0);
    TEST_CHECK(pthread_create(&threads[1], NULL, acquire_loop, &a[1]) == 0);
    TEST_CHECK(pthread_create(&threads[2], NULL, settings_loop, &t) == 0);
    for (i = 0; i < 3; ++i) {
        TEST_CHECK(pthread_join(threads[i], NULL) == 0);
    }
    TEST_CHECK(a[0].status == OST_SUCCESS && a[1].status == OST_SUCCESS);
    TEST_CHECK(a[0].good == ACQUISITIONS_PER_THREAD && a[1].good == ACQUISITIONS_PER_THREAD);
    TEST_CHECK(t.failures == 0u);
    TEST_CHECK(atomic_load(&s.overlaps) == 0);
    TEST_CHECK(s.violations == 0u);
    TEST_CHECK(s.frames_started == 2u * ACQUISITIONS_PER_THREAD);
    ost_camera_close(s.camera);
    TEST_CHECK(atomic_load(&s.closes) == 1);
    TEST_CHECK(atomic_load(&s.after_close) == 0);
    (void)printf("concurrent: %u frames from two threads, %u setter rounds, no overlapping transfer\n",
        a[0].good + a[1].good, SETTER_ROUNDS);
}

/* ---- close while an acquisition runs and another caller waits ------------------------------------------------ */

typedef struct single {
    ost_camera *camera;
    unsigned char *frame;
    size_t acquired;
    int status;
    int after;
    int waiting_status;
} single;

static void *acquire_once(void *argument)
{
    single *r = argument;
    size_t acquired = 7u;

    r->status = ost_camera_acquire(r->camera, r->frame, FRAME_BYTES, &r->acquired);
    r->after = ost_camera_acquire(r->camera, r->frame, FRAME_BYTES, &acquired);
    TEST_CHECK(acquired == 0u);
    return NULL;
}

static void *set_while_busy(void *argument)
{
    single *r = argument;

    r->waiting_status = ost_camera_set_exposure(r->camera, 100u);
    return NULL;
}

static void check_closed_handle(sim *s)
{
    unsigned char frame[16];
    size_t acquired = 7u;
    uint32_t exposure = 0u;
    ost_stream_stats stats;

    TEST_CHECK(ost_camera_acquire(s->camera, frame, sizeof(frame), &acquired) == OST_ERR_CLOSED && acquired == 0u);
    TEST_CHECK(strcmp(ost_camera_last_error(s->camera), "the camera is closed") == 0);
    TEST_CHECK(ost_camera_set_exposure(s->camera, 10u) == OST_ERR_CLOSED);
    TEST_CHECK(ost_camera_get_exposure(s->camera, &exposure) == OST_ERR_CLOSED);
    TEST_CHECK(ost_camera_query_support(s->camera, OST_CAMERA_OP_ACQUIRE) == OST_ERR_CLOSED);
    TEST_CHECK(ost_camera_abort(s->camera) == OST_ERR_CLOSED);
    TEST_CHECK(ost_camera_stream(s->camera, 1u, NULL, NULL, &stats) == OST_ERR_CLOSED);
    TEST_CHECK(stats.frames_delivered == 0u);
    TEST_CHECK(strcmp(ost_status_text(OST_ERR_CLOSED), "camera is closed") == 0);
    ost_camera_close(s->camera);
    TEST_CHECK(atomic_load(&s->closes) == 1);
}

static void test_close_during_acquisition(void)
{
    sim s;
    ost_transport transport;
    pthread_t acquiring;
    pthread_t waiting;
    single r;

    sim_start(&s, &transport);
    memset(&r, 0, sizeof(r));
    r.camera = s.camera;
    r.frame = malloc(FRAME_BYTES);
    TEST_CHECK(r.frame != NULL);
    s.stall = STALL_UNTIL_CLOSING;
    TEST_CHECK(pthread_create(&acquiring, NULL, acquire_once, &r) == 0);
    while (atomic_load(&s.stalled) == 0) {
        sleep_ms(1u);
    }
    TEST_CHECK(pthread_create(&waiting, NULL, set_while_busy, &r) == 0);
    sleep_ms(20u);
    ost_camera_close(s.camera);
    TEST_CHECK(atomic_load(&s.closes) == 1);
    TEST_CHECK(atomic_load(&s.inside) == 0);
    TEST_CHECK(pthread_join(acquiring, NULL) == 0);
    TEST_CHECK(pthread_join(waiting, NULL) == 0);
    TEST_CHECK(r.status == OST_ERR_ABORTED);
    TEST_CHECK(r.acquired > 0u && r.acquired < FRAME_BYTES);
    TEST_CHECK(r.after == OST_ERR_CLOSED);
    TEST_CHECK(r.waiting_status == OST_ERR_CLOSED);
    TEST_CHECK(atomic_load(&s.after_close) == 0);
    TEST_CHECK(atomic_load(&s.overlaps) == 0 && s.violations == 0u);
    check_closed_handle(&s);
    (void)printf("close during acquisition: aborted after %zu of %u bytes, waiting caller got OST_ERR_CLOSED\n",
        r.acquired, (unsigned int)FRAME_BYTES);
    free(r.frame);
}

/* ---- close from the acquiring thread (as a frame callback would) --------------------------------------------- */

static void test_close_from_acquiring_thread(void)
{
    sim s;
    ost_transport transport;
    unsigned char *frame = malloc(FRAME_BYTES);
    size_t acquired = 0u;

    TEST_CHECK(frame != NULL);
    sim_start(&s, &transport);
    s.stall = STALL_CLOSE_HERE;
    TEST_CHECK(ost_camera_acquire(s.camera, frame, FRAME_BYTES, &acquired) == OST_ERR_ABORTED);
    TEST_CHECK(acquired > 0u && acquired < FRAME_BYTES);
    TEST_CHECK(atomic_load(&s.stalled) == 1);
    TEST_CHECK(atomic_load(&s.closes) == 1);
    TEST_CHECK(atomic_load(&s.after_close) == 0);
    check_closed_handle(&s);
    (void)printf("close from the acquiring thread: released when the acquisition returned\n");
    free(frame);
}

/* ---- many cameras opened and closed while another thread uses one ------------------------------------------- */

typedef struct churn {
    ost_camera *camera;
    atomic_int stop;
    unsigned int calls;
} churn;

static void *query_loop(void *argument)
{
    churn *c = argument;

    while (atomic_load(&c->stop) == 0) {
        uint32_t gain = 0u;

        TEST_CHECK(ost_camera_get_gain(c->camera, &gain) == OST_SUCCESS);
        TEST_CHECK(ost_camera_set_exposure(c->camera, 5u) == OST_SUCCESS);
        ++c->calls;
    }
    return NULL;
}

static void test_independent_cameras(void)
{
    sim kept;
    sim other;
    ost_transport kept_transport;
    ost_transport other_transport;
    pthread_t thread;
    churn c;
    unsigned int i;

    sim_start(&kept, &kept_transport);
    memset(&c, 0, sizeof(c));
    c.camera = kept.camera;
    atomic_init(&c.stop, 0);
    TEST_CHECK(pthread_create(&thread, NULL, query_loop, &c) == 0);
    for (i = 0u; i < 50u; ++i) {
        sim_start(&other, &other_transport);
        TEST_CHECK(ost_camera_set_exposure(other.camera, 7u) == OST_SUCCESS);
        ost_camera_close(other.camera);
        TEST_CHECK(atomic_load(&other.closes) == 1);
    }
    atomic_store(&c.stop, 1);
    TEST_CHECK(pthread_join(thread, NULL) == 0);
    ost_camera_close(kept.camera);
    TEST_CHECK(atomic_load(&kept.closes) == 1);
    (void)printf("independent cameras: 50 open/close cycles beside %u calls on another camera\n", c.calls);
}

int main(void)
{
    TEST_CHECK(test_setenv("OPENST133_NO_HARDWARE", "1", 1) == 0);
    test_concurrent_acquisitions();
    test_close_during_acquisition();
    test_close_from_acquiring_thread();
    test_independent_cameras();
    (void)printf("camera thread-safety checks passed\n");
    return EXIT_SUCCESS;
}
