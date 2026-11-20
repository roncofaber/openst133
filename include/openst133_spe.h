/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#ifndef OPENST133_SPE_H
#define OPENST133_SPE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OST_SPE_HEADER_SIZE 4100u

typedef enum {
    OST_SPE_SUCCESS = 0,
    OST_SPE_ERR_INVALID_ARGUMENT = -1,
    OST_SPE_ERR_NO_MEMORY = -2,
    OST_SPE_ERR_IO = -3,
    OST_SPE_ERR_NOT_FOUND = -4,
    OST_SPE_ERR_UNSUPPORTED = -5,
    OST_SPE_ERR_FORMAT = -6,
    OST_SPE_ERR_RANGE = -7
} ost_spe_status;

typedef enum {
    OST_SPE_FLOAT32 = 0,
    OST_SPE_INT32 = 1,
    OST_SPE_INT16 = 2,
    OST_SPE_UINT16 = 3,
    OST_SPE_FLOAT64 = 5,
    OST_SPE_UINT8 = 6,
    OST_SPE_UINT32 = 8
} ost_spe_data_type;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint64_t size;
    uint64_t stride;
} ost_spe_region;

typedef struct {
    unsigned int major_version;
    float header_version;
    ost_spe_data_type data_type;
    uint32_t width;
    uint32_t height;
    uint64_t frame_count;
    uint64_t region_count;
    uint64_t pixel_count;
    size_t pixel_size;
    double exposure_seconds;
    float temperature_setpoint;
    char date[11];
    char software_version[17];
} ost_spe_info;

/* ShutterControl codes WinSpec writes for its Normal, Disabled Closed and Disabled Opened modes. */
typedef enum {
    OST_SPE_SHUTTER_UNKNOWN = 0,
    OST_SPE_SHUTTER_NORMAL = 1,
    OST_SPE_SHUTTER_CLOSED = 2,
    OST_SPE_SHUTTER_OPENED = 3
} ost_spe_shutter;

/*
 * Acquisition provenance. Zero means "not recorded" for the counts, rates, gain, identifiers and
 * sizes; the *_known flags qualify the ROI and temperatures. The ROI is 0-based in sensor pixels,
 * with the binning applied inside it. requested_*_known, lock_*_known and stream_known distinguish
 * zero from unknown; empty target_source and calibration_status strings mean unknown.
 * exposure_seconds and roi are the effective values, retained in their legacy positions.
 * acquired_utc is seconds since the Unix epoch; 0 when writing means the time of writing.
 * date, time_local, time_utc and software_version are filled on read. The extended provenance is
 * serialized in the SPE 3.0 XML footer; ost_spe_get_metadata fills only the legacy C fields.
 */
typedef struct ost_spe_metadata {
    double exposure_seconds;
    uint32_t accumulations;
    int roi_known;
    uint32_t roi_x;
    uint32_t roi_y;
    uint32_t roi_width;
    uint32_t roi_height;
    uint32_t x_binning;
    uint32_t y_binning;
    uint32_t readout_rate_hz;
    uint32_t controller_gain;
    uint32_t detector_id;
    char detector_name[32];
    uint32_t chip_width;
    uint32_t chip_height;
    int temperature_known;
    double temperature_celsius;
    int setpoint_known;
    double setpoint_celsius;
    ost_spe_shutter shutter;
    int64_t acquired_utc;
    char date[11];
    char time_local[7];
    char time_utc[7];
    char software_version[17];
    int requested_exposure_known;
    double requested_exposure_seconds;
    int requested_roi_known;
    uint32_t requested_roi_x;
    uint32_t requested_roi_y;
    uint32_t requested_roi_width;
    uint32_t requested_roi_height;
    uint32_t requested_x_binning;
    uint32_t requested_y_binning;
    char target_source[24];
    int lock_tolerance_known;
    double lock_tolerance_celsius;
    int lock_hold_known;
    double lock_hold_seconds;
    int lock_settle_known;
    double lock_settle_seconds;
    int stream_known;
    uint32_t stream_frames_delivered;
    uint32_t stream_frames_lagged;
    uint32_t stream_host_stalls;
    uint32_t stream_max_ready;
    uint32_t stream_short_transfers;
    uint32_t stream_max_callback_us;
    uint64_t stream_bytes_received;
    uint64_t stream_transfers_completed;
    uint64_t stream_partial_bytes_discarded;
    int stream_stopped;
    char calibration_status[32];
} ost_spe_metadata;

typedef struct ost_spe_file ost_spe_file;

int ost_spe_read(const char *path, ost_spe_file **file);
void ost_spe_free(ost_spe_file *file);
int ost_spe_get_info(const ost_spe_file *file, ost_spe_info *info);
int ost_spe_get_region(const ost_spe_file *file, uint64_t index, ost_spe_region *region);
int ost_spe_read_frame(const ost_spe_file *file, uint64_t index, void *destination,
                       size_t destination_size, size_t *pixel_bytes);
int ost_spe_header(const ost_spe_file *file, const unsigned char **header, size_t *size);
int ost_spe_xml_footer(const ost_spe_file *file, const unsigned char **xml, size_t *size);
int ost_spe_create(const char *path, uint32_t width, uint32_t height,
                   ost_spe_data_type data_type, uint64_t frame_count,
                   const void *frames, size_t frames_size, double exposure_seconds,
                   const float *temperature_setpoint);
int ost_spe_write(const ost_spe_file *file, const char *path);
void ost_spe_metadata_init(ost_spe_metadata *metadata);
int ost_spe_create_with_metadata(const char *path, uint32_t width, uint32_t height,
                                 ost_spe_data_type data_type, uint64_t frame_count,
                                 const void *frames, size_t frames_size,
                                 const ost_spe_metadata *metadata);
int ost_spe_get_metadata(const ost_spe_file *file, ost_spe_metadata *metadata);

#ifdef __cplusplus
}
#endif

#endif
