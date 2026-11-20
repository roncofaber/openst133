/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#include "openst133.h"
#include "openst133_spe.h"
#include "../hw_guard.h"
#include "../pcap_usbmon.h"

#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ost_temperature_main(int argc, char **argv);
int ost_cli_choose_device(
    const char *command, const ost_device_info *devices, size_t count, const char *selector, size_t *index);

enum command {
    COMMAND_LIST,
    COMMAND_INFO,
    COMMAND_SPE_INFO,
    COMMAND_SPE_DUMP,
};

struct options {
    int json;
    int help;
    int has_frame;
    uint64_t frame;
    const char *path;
    const char *camera;
};

static int output(const char *format, ...)
{
    va_list arguments;
    int result;

    va_start(arguments, format);
    result = vfprintf(stdout, format, arguments);
    va_end(arguments);
    return result < 0 ? OST_ERR_IO : OST_SUCCESS;
}

static const char *status_text(int status, int spe)
{
    switch (status) {
    case OST_ERR_INVALID_ARGUMENT:
        return "invalid argument";
    case OST_ERR_NO_MEMORY:
        return "out of memory";
    case OST_ERR_IO:
        return "I/O error";
    case OST_ERR_NOT_FOUND:
        return "not found";
    case OST_ERR_UNSUPPORTED:
        return "unsupported operation";
    case OST_ERR_BUSY:
        return "device is busy";
    case OST_ERR_ACCESS:
        return "permission denied";
    default:
        if (spe && status == OST_SPE_ERR_FORMAT) {
            return "malformed SPE file";
        }
        if (spe && status == OST_SPE_ERR_RANGE) {
            return "frame index out of range";
        }
        return ost_status_text(status);
    }
}

static int report_status(const char *command, int status, int spe)
{
    const char *reason = status_text(status, spe);

    if (status == OST_ERR_UNSUPPORTED && ost_hw_guard_check() != OST_SUCCESS) {
        reason = ost_hw_guard_error();
    }
    fprintf(stderr, "ost: %s: %s (library error %d)\n", command, reason, status);
    return EXIT_FAILURE;
}

static void general_help(void)
{
    puts("Usage: ost COMMAND [OPTIONS]");
    puts("Inspect ST-133 interfaces and SPE files without sending camera commands, except where noted.");
    puts("Commands: list, info, spe-info, spe-dump, temperature, acquire");
    puts("temperature and acquire send camera commands; see 'ost COMMAND --help'.");
    puts("Run 'ost COMMAND --help' for command-specific options.");
}

static void command_usage(enum command command, FILE *stream)
{
    switch (command) {
    case COMMAND_LIST:
        fputs("Usage: ost list [--json]\n", stream);
        break;
    case COMMAND_INFO:
        fputs("Usage: ost info [--json] [--camera SELECTOR]\n", stream);
        break;
    case COMMAND_SPE_INFO:
        fputs("Usage: ost spe-info [--json] FILE\n", stream);
        break;
    case COMMAND_SPE_DUMP:
        fputs("Usage: ost spe-dump [--json] [--frame INDEX] FILE\n", stream);
        break;
    }
}

static void command_help(enum command command)
{
    command_usage(command, stdout);
    switch (command) {
    case COMMAND_LIST:
        puts("List each attached ST-133 interface as VID, PID, bus, address and USB port path (- when unknown),");
        puts("sorted by bus and port path; the line number from 0 is the --camera index.");
        break;
    case COMMAND_INFO:
        puts("Open the ST-133 interface and print its cached descriptor information.");
        puts("--camera SELECTOR  INDEX from 'ost list', BUS-PORT[.PORT] path or BUS:ADDR; required with several cameras");
        break;
    case COMMAND_SPE_INFO:
        puts("Print normalized SPE metadata from FILE.");
        break;
    case COMMAND_SPE_DUMP:
        puts("Print one frame as index,value CSV, or as JSON. INDEX defaults to 0.");
        break;
    }
    puts("--json selects machine-readable output.");
}

static int parse_frame(const char *text, uint64_t *value)
{
    uint64_t result = 0u;
    size_t i;

    if (text[0] == '\0') {
        return 0;
    }
    for (i = 0u; text[i] != '\0'; ++i) {
        unsigned int digit;

        if (text[i] < '0' || text[i] > '9') {
            return 0;
        }
        digit = (unsigned int)(text[i] - '0');
        if (result > (UINT64_MAX - digit) / 10u) {
            return 0;
        }
        result = result * 10u + digit;
    }
    *value = result;
    return 1;
}

static int parse_options(int argc, char **argv, enum command command, struct options *options)
{
    int i;

    memset(options, 0, sizeof(*options));
    for (i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            options->help = 1;
        } else if (strcmp(argv[i], "--json") == 0) {
            options->json = 1;
        } else if (command == COMMAND_SPE_DUMP && strcmp(argv[i], "--frame") == 0) {
            if (++i == argc || !parse_frame(argv[i], &options->frame)) {
                fprintf(stderr, "ost: --frame requires a nonnegative integer\n");
                return 0;
            }
            options->has_frame = 1;
        } else if (command == COMMAND_INFO && strcmp(argv[i], "--camera") == 0) {
            if (++i == argc) {
                fprintf(stderr, "ost: --camera requires an index, BUS-PORT path or BUS:ADDR\n");
                return 0;
            }
            options->camera = argv[i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "ost: unknown option: %s\n", argv[i]);
            return 0;
        } else if (command == COMMAND_SPE_INFO || command == COMMAND_SPE_DUMP) {
            if (options->path != NULL) {
                fprintf(stderr, "ost: only one FILE may be supplied\n");
                return 0;
            }
            options->path = argv[i];
        } else {
            fprintf(stderr, "ost: unexpected argument: %s\n", argv[i]);
            return 0;
        }
    }
    if (options->help) {
        command_help(command);
        return 2;
    }
    if ((command == COMMAND_SPE_INFO || command == COMMAND_SPE_DUMP) && options->path == NULL) {
        fprintf(stderr, "ost: FILE is required\n");
        return 0;
    }
    return 1;
}

static const char *port_path_text(const ost_device_info *info, char *buffer, size_t size)
{
    return ost_device_port_path(info, buffer, size) == OST_SUCCESS ? buffer : "-";
}

static void print_choices(const ost_device_info *devices, size_t count)
{
    char path[64];
    size_t i;

    for (i = 0u; i < count; ++i) {
        fprintf(stderr, "  %zu  %s  %u:%u\n", i, port_path_text(&devices[i], path, sizeof(path)),
            (unsigned int)devices[i].bus_number, (unsigned int)devices[i].device_address);
    }
}

int ost_cli_choose_device(
    const char *command, const ost_device_info *devices, size_t count, const char *selector, size_t *index)
{
    int status = ost_select_device(devices, count, selector, index);

    if (status == OST_SUCCESS) {
        return OST_SUCCESS;
    }
    if (count == 0u) {
        fprintf(stderr, "ost: %s: no ST-133 found\n", command);
    } else if (status == OST_ERR_NOT_FOUND) {
        fprintf(stderr, "ost: %s: no attached ST-133 matches --camera %s; attached (index, port path, bus:address):\n",
            command, selector);
        print_choices(devices, count);
    } else if (selector == NULL || selector[0] == '\0') {
        fprintf(stderr, "ost: %s: %zu ST-133 cameras are attached; choose one with --camera INDEX, BUS-PORT[.PORT] "
            "or BUS:ADDR (index, port path, bus:address):\n", command, count);
        print_choices(devices, count);
    } else {
        fprintf(stderr, "ost: %s: --camera %s is not an index, BUS-PORT[.PORT] path or BUS:ADDR\n", command, selector);
    }
    return status;
}

static int output_info(const ost_device_info *info, int json)
{
    char path[64];
    int status = OST_SUCCESS;
    int has_path = ost_device_port_path(info, path, sizeof(path)) == OST_SUCCESS;

    if (json) {
        status = output("{\"vendor_id\":%u,\"product_id\":%u,\"bus_number\":%u,"
                        "\"device_address\":%u,\"port_path\":%s%s%s}\n",
                        info->vendor_id, info->product_id, info->bus_number, info->device_address,
                        has_path ? "\"" : "", has_path ? path : "null", has_path ? "\"" : "");
    } else {
        status = output("vendor id: 0x%04x\nproduct id: 0x%04x\nbus: %u\naddress: %u\nport path: %s\n",
                        info->vendor_id, info->product_id, info->bus_number, info->device_address,
                        has_path ? path : "-");
    }
    return status;
}

static int list_devices(int json)
{
    ost_device_info *devices = NULL;
    size_t count = 0u;
    size_t i;
    int status = ost_list_devices(NULL, &devices, &count);

    if (status != OST_SUCCESS) {
        return report_status("list", status, 0);
    }
    if (json) {
        status = output("[");
    }
    for (i = 0u; status == OST_SUCCESS && i < count; ++i) {
        char path[64];
        int has_path = ost_device_port_path(&devices[i], path, sizeof(path)) == OST_SUCCESS;

        if (json) {
            status = output("%s{\"vendor_id\":%u,\"product_id\":%u,\"bus_number\":%u,"
                            "\"device_address\":%u,\"port_path\":%s%s%s}",
                            i == 0u ? "" : ",", devices[i].vendor_id, devices[i].product_id,
                            devices[i].bus_number, devices[i].device_address,
                            has_path ? "\"" : "", has_path ? path : "null", has_path ? "\"" : "");
        } else {
            status = output("0x%04x 0x%04x %u %u %s\n", devices[i].vendor_id, devices[i].product_id,
                            devices[i].bus_number, devices[i].device_address, has_path ? path : "-");
        }
    }
    if (status == OST_SUCCESS && json) {
        status = output("]\n");
    }
    ost_free_devices(devices);
    if (status != OST_SUCCESS) {
        fprintf(stderr, "ost: list: standard output: %s\n", status_text(status, 0));
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

static int device_info(int json, const char *camera)
{
    ost_device_info *devices = NULL;
    ost_device_info info;
    ost_device *device = NULL;
    size_t count = 0u;
    size_t index = 0u;
    int status = ost_list_devices(NULL, &devices, &count);

    if (status != OST_SUCCESS) {
        return report_status("info", status, 0);
    }
    if (count == 0u) {
        ost_free_devices(devices);
        return report_status("info", OST_ERR_NOT_FOUND, 0);
    }
    if (ost_cli_choose_device("info", devices, count, camera, &index) != OST_SUCCESS) {
        ost_free_devices(devices);
        return EXIT_FAILURE;
    }
    status = ost_open(NULL, &devices[index], &device);
    ost_free_devices(devices);
    if (status == OST_SUCCESS) {
        status = ost_get_info(device, &info);
        ost_close(device);
    }
    if (status != OST_SUCCESS) {
        return report_status("info", status, 0);
    }
    status = output_info(&info, json);
    if (status != OST_SUCCESS) {
        fprintf(stderr, "ost: info: standard output: %s\n", status_text(status, 0));
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

static const char *data_type_name(ost_spe_data_type data_type)
{
    switch (data_type) {
    case OST_SPE_FLOAT32:
        return "float32";
    case OST_SPE_INT32:
        return "int32";
    case OST_SPE_INT16:
        return "int16";
    case OST_SPE_UINT16:
        return "uint16";
    case OST_SPE_FLOAT64:
        return "float64";
    case OST_SPE_UINT8:
        return "uint8";
    case OST_SPE_UINT32:
        return "uint32";
    }
    return "unknown";
}

static int json_string(const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;
    int status = output("\"");

    while (status == OST_SUCCESS && *cursor != 0u) {
        switch (*cursor) {
        case '"':
            status = output("\\\"");
            break;
        case '\\':
            status = output("\\\\");
            break;
        case '\b':
            status = output("\\b");
            break;
        case '\f':
            status = output("\\f");
            break;
        case '\n':
            status = output("\\n");
            break;
        case '\r':
            status = output("\\r");
            break;
        case '\t':
            status = output("\\t");
            break;
        default:
            if (*cursor < 0x20u || *cursor >= 0x80u) {
                status = output("\\u%04x", *cursor);
            } else {
                status = output("%c", *cursor);
            }
            break;
        }
        ++cursor;
    }
    return status == OST_SUCCESS ? output("\"") : status;
}

static int spe_info_values(const ost_spe_info *info, int json)
{
    int status;

    if (json) {
        status = output("{\"major_version\":%u,\"header_version\":%.9g,"
                        "\"data_type\":\"%s\",\"width\":%u,\"height\":%u,"
                        "\"frame_count\":%" PRIu64 ",\"region_count\":%" PRIu64
                        ",\"pixel_count\":%" PRIu64 ",\"pixel_size\":%zu,"
                        "\"exposure_seconds\":%.17g,\"date\":",
                        info->major_version, (double)info->header_version,
                        data_type_name(info->data_type), info->width, info->height, info->frame_count,
                        info->region_count, info->pixel_count, info->pixel_size, info->exposure_seconds);
        if (status == OST_SUCCESS) {
            status = json_string(info->date);
        }
        if (status == OST_SUCCESS) {
            status = output(",\"software_version\":");
        }
        if (status == OST_SUCCESS) {
            status = json_string(info->software_version);
        }
        return status == OST_SUCCESS ? output("}\n") : status;
    }
    return output("major version: %u\nheader version: %.9g\ndata type: %s\n"
                  "width: %u\nheight: %u\nframes: %" PRIu64 "\nregions: %" PRIu64
                  "\npixels per frame: %" PRIu64 "\nbytes per pixel: %zu\n"
                  "exposure seconds: %.17g\ndate: %s\nsoftware version: %s\n",
                  info->major_version, (double)info->header_version, data_type_name(info->data_type),
                  info->width, info->height, info->frame_count, info->region_count, info->pixel_count,
                  info->pixel_size, info->exposure_seconds, info->date, info->software_version);
}

static int spe_info(const char *path, int json)
{
    ost_spe_file *file = NULL;
    ost_spe_info info;
    int status = ost_spe_read(path, &file);

    if (status == OST_SPE_SUCCESS) {
        status = ost_spe_get_info(file, &info);
    }
    if (status == OST_SPE_SUCCESS) {
        status = spe_info_values(&info, json);
    }
    ost_spe_free(file);
    if (status != OST_SPE_SUCCESS) {
        if (status == OST_SPE_ERR_IO && ferror(stdout)) {
            fprintf(stderr, "ost: spe-info: standard output: I/O error\n");
            return EXIT_FAILURE;
        }
        return report_status("spe-info", status, 1);
    }
    return EXIT_SUCCESS;
}

static int write_pixel(const unsigned char *bytes, const ost_spe_info *info, int json)
{
    switch (info->data_type) {
    case OST_SPE_FLOAT32: {
        float value;
        memcpy(&value, bytes, sizeof(value));
        return json && !isfinite(value) ? output("null") : output("%.9g", (double)value);
    }
    case OST_SPE_INT32: {
        int32_t value;
        memcpy(&value, bytes, sizeof(value));
        return output("%" PRId32, value);
    }
    case OST_SPE_INT16: {
        int16_t value;
        memcpy(&value, bytes, sizeof(value));
        return output("%" PRId16, value);
    }
    case OST_SPE_UINT16: {
        uint16_t value;
        memcpy(&value, bytes, sizeof(value));
        return output("%" PRIu16, value);
    }
    case OST_SPE_FLOAT64: {
        double value;
        memcpy(&value, bytes, sizeof(value));
        return json && !isfinite(value) ? output("null") : output("%.17g", value);
    }
    case OST_SPE_UINT8:
        return output("%u", *bytes);
    case OST_SPE_UINT32: {
        uint32_t value;
        memcpy(&value, bytes, sizeof(value));
        return output("%" PRIu32, value);
    }
    }
    return OST_SPE_ERR_UNSUPPORTED;
}

static int spe_dump(const char *path, uint64_t frame, int json)
{
    ost_spe_file *file = NULL;
    ost_spe_info info;
    unsigned char *pixels = NULL;
    size_t buffer_size;
    size_t pixel_bytes = 0u;
    uint64_t i;
    int status = ost_spe_read(path, &file);

    if (status == OST_SPE_SUCCESS) {
        status = ost_spe_get_info(file, &info);
    }
    if (status != OST_SPE_SUCCESS) {
        ost_spe_free(file);
        return report_status("spe-dump", status, 1);
    }
    if (info.pixel_count > SIZE_MAX / info.pixel_size) {
        ost_spe_free(file);
        return report_status("spe-dump", OST_SPE_ERR_FORMAT, 1);
    }
    buffer_size = (size_t)info.pixel_count * info.pixel_size;
    pixels = malloc(buffer_size);
    if (pixels == NULL) {
        ost_spe_free(file);
        return report_status("spe-dump", OST_SPE_ERR_NO_MEMORY, 1);
    }
    status = ost_spe_read_frame(file, frame, pixels, buffer_size, &pixel_bytes);
    if (status == OST_SPE_SUCCESS && pixel_bytes != buffer_size) {
        status = OST_SPE_ERR_FORMAT;
    }
    if (status == OST_SPE_SUCCESS && !json) {
        status = output("index,value\n");
    } else if (status == OST_SPE_SUCCESS) {
        status = output("{\"frame\":%" PRIu64 ",\"data_type\":\"%s\","
                        "\"pixel_count\":%" PRIu64 ",\"pixels\":[",
                        frame, data_type_name(info.data_type), info.pixel_count);
    }
    for (i = 0u; status == OST_SUCCESS && i < info.pixel_count; ++i) {
        if (!json) {
            status = output("%" PRIu64 ",", i);
        } else if (i != 0u) {
            status = output(",");
        }
        if (status == OST_SUCCESS) {
            status = write_pixel(pixels + (size_t)i * info.pixel_size, &info, json);
        }
        if (status == OST_SUCCESS && !json) {
            status = output("\n");
        }
    }
    if (status == OST_SUCCESS && json) {
        status = output("]}\n");
    }
    free(pixels);
    ost_spe_free(file);
    if (status != OST_SUCCESS) {
        if (status == OST_ERR_IO && ferror(stdout)) {
            fprintf(stderr, "ost: spe-dump: standard output: I/O error\n");
            return EXIT_FAILURE;
        }
        return report_status("spe-dump", status, 1);
    }
    return EXIT_SUCCESS;
}

static void acquire_usage(FILE *stream)
{
    fputs("Usage: ost acquire --exposure SECONDS --out FILE.spe "
          "[--strict-exposure] [--count N] [--closed | --shutter MODE] [--gain N] [--accumulations N] [--stream] "
          "[--setpoint C] [--fvb] [--roi X,Y,W,H] [--bin XxY] [--rate 1M|100k] [--camera SELECTOR]\n",
        stream);
}

static int parse_u32_list(const char *text, char separator, uint32_t *values, size_t count)
{
    size_t i;

    for (i = 0u; i < count; ++i) {
        uint32_t value = 0u;
        size_t digits = 0u;

        while (text[digits] >= '0' && text[digits] <= '9') {
            unsigned int digit = (unsigned int)(text[digits] - '0');

            if (value > (UINT32_MAX - digit) / 10u) {
                return 0;
            }
            value = value * 10u + digit;
            ++digits;
        }
        if (digits == 0u) {
            return 0;
        }
        values[i] = value;
        text += digits;
        if (i + 1u < count) {
            if (*text != separator) {
                return 0;
            }
            ++text;
        } else if (*text != '\0') {
            return 0;
        }
    }
    return 1;
}

static int parse_exposure_ms(const char *text, unsigned int *milliseconds)
{
    char *end = NULL;
    double seconds;

    if (text[0] == '\0') {
        return 0;
    }
    seconds = strtod(text, &end);
    if (end == text || *end != '\0' || !isfinite(seconds) || seconds < 0.0 || seconds > 1000.0) {
        return 0;
    }
    *milliseconds = (unsigned int)(seconds * 1000.0 + 0.5);
    return seconds == 0.0 || *milliseconds != 0u;
}

static unsigned int next_exposure_representation(unsigned int requested_ms)
{
    unsigned int scale = 0u;

    while (((requested_ms + (1u << scale) - 1u) >> scale) > 255u) {
        ++scale;
    }
    return ((requested_ms >> scale) + 1u) << scale;
}

typedef struct acquire_collection {
    unsigned char *frames;
    size_t frame_size;
    uint32_t frame_count;
    int keep_last;
} acquire_collection;

static ost_camera *volatile stream_camera;
static volatile sig_atomic_t stream_interrupted;

static void stream_interrupt(int signal_number)
{
    ost_camera *camera = stream_camera;

    (void)signal_number;
    /* An abort that lands before ost_camera_stream claims the camera is cleared by the claim; the callback stops too. */
    stream_interrupted = 1;
    if (camera != NULL) {
        (void)ost_camera_abort(camera);
    }
}

static int collect_acquired_frame(void *userdata, uint32_t index, const void *frame, size_t frame_size)
{
    acquire_collection *collection = userdata;

    if (frame_size != collection->frame_size || (!collection->keep_last && index >= collection->frame_count)) {
        return OST_ERR_IO;
    }
    memcpy(collection->frames + (collection->keep_last ? 0u : (size_t)index * frame_size), frame, frame_size);
    return stream_interrupted ? OST_ERR_ABORTED : OST_SUCCESS;
}

typedef struct acquire_geometry_flags {
    int fvb;
    int has_roi;
    uint32_t roi[4];
    int has_binning;
    uint32_t binning[2];
} acquire_geometry_flags;

static int acquire_geometry(const acquire_geometry_flags *flags, uint32_t sensor_width, uint32_t sensor_height,
    const ost_camera_readout *current, ost_camera_readout *readout, char *message, size_t message_size)
{
    *readout = *current;
    message[0] = '\0';
    if (flags->has_roi) {
        readout->x = flags->roi[0];
        readout->y = flags->roi[1];
        readout->width = flags->roi[2];
        readout->height = flags->roi[3];
    } else if (flags->fvb) {
        readout->x = 0u;
        readout->y = 0u;
        readout->width = sensor_width;
        readout->height = sensor_height;
    }
    if (flags->has_binning) {
        readout->x_binning = flags->binning[0];
    } else if (flags->fvb) {
        readout->x_binning = 1u;
    }
    if (!flags->fvb) {
        if (flags->has_binning) {
            readout->y_binning = flags->binning[1];
        }
        return 1;
    }
    if (flags->has_binning && flags->binning[1] != 1u && flags->binning[1] != readout->height) {
        (void)snprintf(message, message_size,
            "--fvb bins all %u rows of the region into one spectrum, so --bin %ux%u conflicts with it; use --bin %ux1 "
            "(the same as --bin %ux%u) for horizontal binning %u with --fvb, or drop --fvb to bin %u rows",
            readout->height, flags->binning[0], flags->binning[1], flags->binning[0], flags->binning[0],
            readout->height, flags->binning[0], flags->binning[1]);
        return 0;
    }
    readout->y_binning = readout->height;
    return 1;
}

static int acquire_apply_geometry(ost_camera *camera, const acquire_geometry_flags *flags, char *message,
    size_t message_size)
{
    ost_sensor_geometry sensor;
    ost_camera_readout current;
    ost_camera_readout readout;
    int status;

    message[0] = '\0';
    if (!flags->fvb && !flags->has_roi && !flags->has_binning) {
        return OST_SUCCESS;
    }
    status = ost_camera_get_geometry(camera, &sensor);
    if (status == OST_SUCCESS) {
        status = ost_camera_get_readout(camera, &current);
    }
    if (status != OST_SUCCESS) {
        return status;
    }
    if (!acquire_geometry(flags, sensor.width, sensor.height, &current, &readout, message, message_size)) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    return ost_camera_set_readout(camera, &readout);
}

static int acquire_main(int argc, char **argv)
{
    const char *out = NULL;
    const char *replay_path = NULL;
    const char *camera_selector = NULL;
    unsigned int exposure_ms = 0u;
    unsigned int effective_exposure_ms = 0u;
    uint32_t frame_count = 1u;
    uint32_t frames_read = 0u;
    acquire_geometry_flags geometry = {0, 0, {0u, 0u, 0u, 0u}, 0, {1u, 1u}};
    ost_camera_readout default_readout = {
        0u, 0u, OST_CAMERA_DEFAULT_WIDTH, OST_CAMERA_DEFAULT_HEIGHT, 1u, 1u
    };
    char geometry_message[320];
    uint32_t readout_rate_hz = 0u;
    double requested_setpoint = 0.0;
    int has_setpoint = 0;
    int has_rate = 0;
    ost_shutter_state shutter = OST_SHUTTER_OPEN;
    const char *shutter_name = "normal";
    uint32_t gain = 0u;
    uint32_t accumulations = 0u;
    size_t sample_bytes;
    int streaming = 0;
    int strict_exposure = 0;
    int has_exposure = 0;
    int count_given = 0;
    ost_stream_stats stats;
    ost_replay_transport *replay = NULL;
    const ost_transport *transport = NULL;
    ost_device_info *devices = NULL;
    size_t count = 0u;
    size_t device_index = 0u;
    ost_camera *camera = NULL;
    ost_camera_identity identity;
    ost_camera_readout readout;
    ost_camera_readout requested_readout;
    ost_camera_target_source target_source;
    int requested_readout_known;
    unsigned char *frame;
    size_t output_width;
    size_t output_height;
    size_t frame_size;
    size_t frames_size;
    size_t acquired_size = 0u;
    acquire_collection collection;
    ost_spe_metadata metadata;
    char error[256];
    int status;
    int i;

    for (i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            acquire_usage(stdout);
            puts("Acquire from the ST-133 and save the selected readout as SPE.");
            puts("Startup data from 'openst133 setup' is required (docs/user/startup-data.md). The library validates");
            puts("every setting against its allowed range and refuses unsupported values and combinations.");
            puts("--exposure SECONDS  exposure duration, rounded to the nearest millisecond");
            puts("--strict-exposure   refuse a duration whose generated count/scale representation is shorter");
            puts("--count N           repeat the exposure N times in one multi-frame SPE (default 1)");
            puts("--stream            stream frames as they complete; with --count N stop after N frames and save all of");
            puts("                    them, without it run until Ctrl-C and save the last frame");
            puts("                    the camera stop sequence is sent at the end and dropped or partial data is reported");
            puts("--closed            Disabled Closed shutter instead of Normal (same as --shutter closed)");
            puts("--shutter MODE      normal, closed, opened (Disabled Opened) or preopen (refused:");
            puts("                    Pre Open requires External Sync timing, which is unsupported)");
            puts("--gain N            controller gain 1, 2 or 3 (default 2)");
            puts("--accumulations N   frames the camera reads and ost sums into one 32-bit (LONG) SPE frame;");
            puts("                    1, 3 or 10");
            puts("--setpoint C        requested temperature setpoint in degrees Celsius");
            puts("--fvb               full-vertical binning: every row of the region (the whole sensor without --roi)");
            puts("                    summed into one spectrum; add --bin Nx1 for horizontal binning N");
            puts("--roi X,Y,W,H       sensor-pixel region of interest");
            puts("--bin XxY           horizontal and vertical integer binning (with --fvb only Nx1 or Nx<rows>)");
            puts("--rate 1M|100k      readout rate");
            puts("--camera SELECTOR   INDEX from 'ost list', BUS-PORT[.PORT] path or BUS:ADDR; required with several cameras");
            puts("--replay PCAP       diagnostic: replay a prepared usbmon capture instead of using USB");
            return EXIT_SUCCESS;
        } else if (strcmp(argv[i], "--setpoint") == 0) {
            char *end = NULL;

            if (++i == argc) {
                fputs("ost: --setpoint requires degrees Celsius\n", stderr);
                return EXIT_FAILURE;
            }
            requested_setpoint = strtod(argv[i], &end);
            if (end == argv[i] || *end != '\0' || !isfinite(requested_setpoint)) {
                fputs("ost: --setpoint requires degrees Celsius\n", stderr);
                return EXIT_FAILURE;
            }
            has_setpoint = 1;
        } else if (strcmp(argv[i], "--fvb") == 0) {
            geometry.fvb = 1;
        } else if (strcmp(argv[i], "--roi") == 0) {
            if (++i == argc || !parse_u32_list(argv[i], ',', geometry.roi, 4u)) {
                fputs("ost: --roi requires X,Y,W,H as unsigned 32-bit integers\n", stderr);
                return EXIT_FAILURE;
            }
            geometry.has_roi = 1;
        } else if (strcmp(argv[i], "--bin") == 0) {
            if (++i == argc || !parse_u32_list(argv[i], 'x', geometry.binning, 2u)) {
                fputs("ost: --bin requires XxY as unsigned 32-bit integers\n", stderr);
                return EXIT_FAILURE;
            }
            geometry.has_binning = 1;
        } else if (strcmp(argv[i], "--rate") == 0) {
            if (++i == argc || (strcmp(argv[i], "1M") != 0 && strcmp(argv[i], "100k") != 0)) {
                fputs("ost: --rate requires 1M or 100k\n", stderr);
                return EXIT_FAILURE;
            }
            readout_rate_hz = strcmp(argv[i], "1M") == 0 ? 1000000u : 100000u;
            has_rate = 1;
        } else if (strcmp(argv[i], "--closed") == 0) {
            shutter = OST_SHUTTER_CLOSED;
            shutter_name = "closed";
        } else if (strcmp(argv[i], "--shutter") == 0) {
            static const char *const names[] = {"closed", "normal", "opened", "preopen"};
            size_t k;

            if (++i == argc) {
                fputs("ost: --shutter requires normal, closed, opened or preopen\n", stderr);
                return EXIT_FAILURE;
            }
            for (k = 0u; k < sizeof(names) / sizeof(names[0]) && strcmp(argv[i], names[k]) != 0; ++k) {
            }
            if (k == sizeof(names) / sizeof(names[0])) {
                fputs("ost: --shutter requires normal, closed, opened or preopen\n", stderr);
                return EXIT_FAILURE;
            }
            shutter = (ost_shutter_state)k;
            shutter_name = names[k];
        } else if (strcmp(argv[i], "--gain") == 0 || strcmp(argv[i], "--accumulations") == 0) {
            uint64_t parsed;
            int is_gain = strcmp(argv[i], "--gain") == 0;

            if (++i == argc || !parse_frame(argv[i], &parsed) || parsed == 0u || parsed > UINT32_MAX) {
                fprintf(stderr, "ost: %s requires a positive 32-bit integer\n", is_gain ? "--gain" : "--accumulations");
                return EXIT_FAILURE;
            }
            if (is_gain) {
                gain = (uint32_t)parsed;
            } else {
                accumulations = (uint32_t)parsed;
            }
        } else if (strcmp(argv[i], "--stream") == 0) {
            streaming = 1;
        } else if (strcmp(argv[i], "--strict-exposure") == 0) {
            strict_exposure = 1;
        } else if (strcmp(argv[i], "--exposure") == 0) {
            if (++i == argc || !parse_exposure_ms(argv[i], &exposure_ms)) {
                fputs("ost: --exposure requires a nonnegative duration in seconds\n", stderr);
                return EXIT_FAILURE;
            }
            has_exposure = 1;
        } else if (strcmp(argv[i], "--count") == 0) {
            uint64_t parsed_count;

            if (++i == argc || !parse_frame(argv[i], &parsed_count) || parsed_count == 0u ||
                parsed_count > UINT32_MAX) {
                fputs("ost: --count requires a positive 32-bit integer\n", stderr);
                return EXIT_FAILURE;
            }
            frame_count = (uint32_t)parsed_count;
            count_given = 1;
        } else if (strcmp(argv[i], "--out") == 0) {
            if (++i == argc) {
                fputs("ost: --out requires a file name\n", stderr);
                return EXIT_FAILURE;
            }
            out = argv[i];
        } else if (strcmp(argv[i], "--replay") == 0) {
            if (++i == argc) {
                fputs("ost: --replay requires a capture\n", stderr);
                return EXIT_FAILURE;
            }
            replay_path = argv[i];
        } else if (strcmp(argv[i], "--camera") == 0) {
            if (++i == argc) {
                fputs("ost: --camera requires an index, BUS-PORT path or BUS:ADDR\n", stderr);
                return EXIT_FAILURE;
            }
            camera_selector = argv[i];
        } else {
            fprintf(stderr, "ost: unknown option: %s\n", argv[i]);
            acquire_usage(stderr);
            return EXIT_FAILURE;
        }
    }
    if (!has_exposure || out == NULL) {
        fputs("ost: acquire: --exposure and --out are required\n", stderr);
        acquire_usage(stderr);
        return EXIT_FAILURE;
    }
    if (!acquire_geometry(&geometry, OST_CAMERA_DEFAULT_WIDTH, OST_CAMERA_DEFAULT_HEIGHT, &default_readout,
            &readout, geometry_message, sizeof(geometry_message))) {
        fprintf(stderr, "ost: acquire: %s\n", geometry_message);
        return EXIT_FAILURE;
    }
    requested_readout = readout;
    requested_readout_known = geometry.has_roi || geometry.has_binning || geometry.fvb;
    if (replay_path != NULL) {
        if (ost_replay_transport_create(replay_path, &replay, error, sizeof(error)) != OST_SUCCESS) {
            fprintf(stderr, "ost: acquire: replay: %s\n", error);
            return EXIT_FAILURE;
        }
        transport = ost_replay_transport_interface(replay);
    }
    status = ost_list_devices(transport, &devices, &count);
    if (status != OST_SUCCESS) {
        ost_free_devices(devices);
        ost_replay_transport_destroy(replay);
        fprintf(stderr, "ost: acquire: %s\n", status_text(status, 0));
        return EXIT_FAILURE;
    }
    if (ost_cli_choose_device("acquire", devices, count, camera_selector, &device_index) != OST_SUCCESS) {
        ost_free_devices(devices);
        ost_replay_transport_destroy(replay);
        return EXIT_FAILURE;
    }
    status = ost_camera_open(transport, &devices[device_index], &camera);
    ost_free_devices(devices);
    if (status != OST_SUCCESS) {
        fprintf(stderr, "ost: acquire: open failed: %s (library error %d)%s%s\n", status_text(status, 0), status,
            ost_camera_open_error()[0] != '\0' ? ": " : "", ost_camera_open_error());
        ost_replay_transport_destroy(replay);
        return EXIT_FAILURE;
    }
    status = ost_camera_set_exposure(camera, exposure_ms);
    if (status == OST_SUCCESS) {
        status = ost_camera_get_effective_exposure(camera, &effective_exposure_ms);
    }
    if (status == OST_SUCCESS && strict_exposure && effective_exposure_ms != exposure_ms) {
        fprintf(stderr, "ost: acquire: exposure %u ms is not exactly representable; nearest count/scale values are "
            "%u ms and %u ms; without --strict-exposure the acquisition uses %u ms\n", exposure_ms,
            effective_exposure_ms, next_exposure_representation(exposure_ms), effective_exposure_ms);
        ost_camera_close(camera);
        ost_replay_transport_destroy(replay);
        return EXIT_FAILURE;
    }
    if (status == OST_SUCCESS && shutter != OST_SHUTTER_OPEN) {
        status = ost_camera_set_shutter(camera, shutter);
    }
    if (status == OST_SUCCESS && gain != 0u) {
        status = ost_camera_set_gain(camera, gain);
    }
    if (status == OST_SUCCESS && accumulations != 0u) {
        status = ost_camera_set_accumulations(camera, accumulations);
    }
    if (status == OST_SUCCESS) {
        status = ost_camera_get_gain(camera, &gain);
    }
    if (status == OST_SUCCESS) {
        status = ost_camera_get_accumulations(camera, &accumulations);
    }
    if (status == OST_SUCCESS) {
        status = acquire_apply_geometry(camera, &geometry, geometry_message, sizeof(geometry_message));
        if (geometry_message[0] != '\0') {
            fprintf(stderr, "ost: acquire: %s\n", geometry_message);
            ost_camera_close(camera);
            ost_replay_transport_destroy(replay);
            return EXIT_FAILURE;
        }
    }
    if (status == OST_SUCCESS && has_rate) {
        status = ost_camera_set_readout_rate(camera, readout_rate_hz);
    }
    if (status == OST_SUCCESS && has_setpoint) {
        status = ost_camera_set_temperature(camera, requested_setpoint);
    }
    if (status == OST_SUCCESS) {
        status = ost_camera_get_readout(camera, &readout);
    }
    if (status != OST_SUCCESS) {
        fprintf(stderr, "ost: acquire: %s (library error %d): %s\n", status_text(status, 0), status,
            ost_camera_last_error(camera));
        ost_camera_close(camera);
        ost_replay_transport_destroy(replay);
        return EXIT_FAILURE;
    }
    output_width = (size_t)(readout.width / readout.x_binning);
    output_height = (size_t)(readout.height / readout.y_binning);
    if (output_width == 0u) {
        output_width = 1u;
    }
    if (output_height == 0u) {
        output_height = 1u;
    }
    sample_bytes = accumulations > 1u ? sizeof(int32_t) : OST_CAMERA_BYTES_PER_PIXEL;
    if ((output_width != 0u && output_height > SIZE_MAX / output_width) ||
        output_width * output_height > SIZE_MAX / sample_bytes) {
        ost_camera_close(camera);
        ost_replay_transport_destroy(replay);
        return report_status("acquire", OST_ERR_NO_MEMORY, 0);
    }
    frame_size = output_width * output_height * sample_bytes;
    if (streaming && !count_given) {
        frame_count = 1u;
    }
    if (frame_size == 0u || frame_count > SIZE_MAX / frame_size) {
        ost_camera_close(camera);
        ost_replay_transport_destroy(replay);
        return report_status("acquire", OST_ERR_NO_MEMORY, 0);
    }
    frames_size = frame_size * frame_count;
    frame = malloc(frames_size);
    if (frame == NULL) {
        ost_camera_close(camera);
        ost_replay_transport_destroy(replay);
        return report_status("acquire", OST_ERR_NO_MEMORY, 0);
    }
    collection.frames = frame;
    collection.frame_size = frame_size;
    collection.frame_count = frame_count;
    collection.keep_last = streaming && !count_given;
    if (streaming) {
        stream_interrupted = 0;
        stream_camera = camera;
        (void)signal(SIGINT, stream_interrupt);
        status = ost_camera_stream(camera, count_given ? frame_count : 0u, collect_acquired_frame, &collection, &stats);
        (void)signal(SIGINT, SIG_DFL);
        stream_camera = NULL;
        frames_read = count_given ? stats.frames_delivered : (stats.frames_delivered != 0u ? 1u : 0u);
        fprintf(stderr, "ost: stream: %" PRIu32 " frames delivered, %" PRIu64 " partial bytes discarded, %" PRIu32
            " host stalls, %" PRIu32 " lagging frames, %s\n", stats.frames_delivered, stats.partial_bytes_discarded,
            stats.host_stalls, stats.frames_lagged, stats.stopped ? "stopped on request" : "count reached");
        if (status == OST_SUCCESS && frames_read == 0u) {
            fputs("ost: acquire: no complete frame arrived\n", stderr);
            status = OST_ERR_IO;
        }
    } else if (frame_count == 1u) {
        status = ost_camera_acquire(camera, frame, frame_size, &acquired_size);
        if (status == OST_SUCCESS && acquired_size != frame_size) {
            status = OST_ERR_IO;
        }
        frames_read = status == OST_SUCCESS ? 1u : 0u;
    } else {
        frames_read = 0u;
        status = OST_SUCCESS;
        while (status == OST_SUCCESS && frames_read < frame_count) {
            status = ost_camera_acquire(camera, frame + (size_t)frames_read * frame_size, frame_size, &acquired_size);
            if (status == OST_SUCCESS && acquired_size != frame_size) {
                status = OST_ERR_IO;
            }
            frames_read += status == OST_SUCCESS ? 1u : 0u;
        }
    }
    if (status != OST_SUCCESS) {
        fprintf(stderr, "ost: acquire: %s (library error %d): %s\n", status_text(status, 0), status,
            ost_camera_last_error(camera));
        free(frame);
        ost_camera_close(camera);
        ost_replay_transport_destroy(replay);
        return EXIT_FAILURE;
    }
    ost_spe_metadata_init(&metadata);
    metadata.exposure_seconds = (double)effective_exposure_ms / 1000.0;
    metadata.requested_exposure_known = 1;
    metadata.requested_exposure_seconds = (double)exposure_ms / 1000.0;
    metadata.requested_roi_known = requested_readout_known;
    metadata.requested_roi_x = requested_readout.x;
    metadata.requested_roi_y = requested_readout.y;
    metadata.requested_roi_width = requested_readout.width;
    metadata.requested_roi_height = requested_readout.height;
    metadata.requested_x_binning = requested_readout.x_binning;
    metadata.requested_y_binning = requested_readout.y_binning;
    metadata.accumulations = accumulations;
    metadata.controller_gain = gain;
    metadata.roi_known = 1;
    metadata.roi_x = readout.x;
    metadata.roi_y = readout.y;
    metadata.roi_width = readout.width;
    metadata.roi_height = readout.height;
    metadata.x_binning = readout.x_binning;
    metadata.y_binning = readout.y_binning;
    status = ost_camera_get_target_source(camera, &target_source);
    if (status != OST_SUCCESS) {
        fprintf(stderr, "ost: acquire: cannot read target source for SPE metadata\n");
        free(frame);
        ost_camera_close(camera);
        ost_replay_transport_destroy(replay);
        return EXIT_FAILURE;
    }
    if (target_source == OST_CAMERA_TARGET_SOURCE_NONE) {
        (void)snprintf(metadata.target_source, sizeof(metadata.target_source), "%s", "none");
    } else if (target_source == OST_CAMERA_TARGET_SOURCE_OPTION) {
        (void)snprintf(metadata.target_source, sizeof(metadata.target_source), "%s", "option");
    } else if (target_source == OST_CAMERA_TARGET_SOURCE_PERSISTED) {
        (void)snprintf(metadata.target_source, sizeof(metadata.target_source), "%s", "persisted");
    } else if (target_source == OST_CAMERA_TARGET_SOURCE_CAMERA_DEFAULT) {
        (void)snprintf(metadata.target_source, sizeof(metadata.target_source), "%s", "camera-default");
    } else if (target_source == OST_CAMERA_TARGET_SOURCE_STARTUP_DEFAULT) {
        (void)snprintf(metadata.target_source, sizeof(metadata.target_source), "%s", "startup-default");
    } else {
        free(frame);
        ost_camera_close(camera);
        ost_replay_transport_destroy(replay);
        return report_status("acquire", OST_ERR_UNSUPPORTED, 1);
    }
    metadata.readout_rate_hz = has_rate ? readout_rate_hz : 1000000u;
    metadata.shutter = shutter == OST_SHUTTER_CLOSED ? OST_SPE_SHUTTER_CLOSED
        : shutter == OST_SHUTTER_DISABLED_OPENED ? OST_SPE_SHUTTER_OPENED : OST_SPE_SHUTTER_NORMAL;
    if (streaming) {
        metadata.stream_known = 1;
        metadata.stream_frames_delivered = stats.frames_delivered;
        metadata.stream_frames_lagged = stats.frames_lagged;
        metadata.stream_host_stalls = stats.host_stalls;
        metadata.stream_max_ready = stats.max_ready;
        metadata.stream_short_transfers = stats.short_transfers;
        metadata.stream_max_callback_us = stats.max_callback_us;
        metadata.stream_bytes_received = stats.bytes_received;
        metadata.stream_transfers_completed = stats.transfers_completed;
        metadata.stream_partial_bytes_discarded = stats.partial_bytes_discarded;
        metadata.stream_stopped = stats.stopped;
    }
    if (ost_camera_get_identity(camera, &identity) == OST_SUCCESS) {
        metadata.detector_id = identity.detector_id;
        (void)snprintf(metadata.detector_name, sizeof(metadata.detector_name), "%s", identity.detector_name);
        metadata.chip_width = identity.width;
        metadata.chip_height = identity.height;
        metadata.setpoint_known = identity.target_known;
        metadata.setpoint_celsius = identity.target_celsius;
    }
    status = ost_spe_create_with_metadata(out, (uint32_t)output_width, (uint32_t)output_height,
        accumulations > 1u ? OST_SPE_INT32 : OST_SPE_UINT16, frames_read, frame, frame_size * frames_read, &metadata);
    free(frame);
    ost_camera_close(camera);
    ost_replay_transport_destroy(replay);
    if (status != OST_SPE_SUCCESS) {
        return report_status("acquire", status, 1);
    }
    if (frames_read == 1u) {
        printf("acquired %zux%zu frame, exposure %.3f s", output_width, output_height,
            (double)effective_exposure_ms / 1000.0);
    } else {
        printf("acquired %zux%zu %" PRIu32 " frames, exposure %.3f s", output_width, output_height, frames_read,
            (double)effective_exposure_ms / 1000.0);
    }
    if (effective_exposure_ms != exposure_ms) {
        printf(" (requested %g s)", (double)exposure_ms / 1000.0);
    }
    printf(", %s shutter, gain %" PRIu32, shutter_name, gain);
    if (accumulations > 1u) {
        printf(", %" PRIu32 " accumulations summed", accumulations);
    }
    printf(", saved %s\n", out);
    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    enum command command;
    const char *name;
    struct options options;
    int parsed;
    int result;

    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        general_help();
        return EXIT_SUCCESS;
    }
    if (argc < 2) {
        fputs("Usage: ost COMMAND [OPTIONS]\n", stderr);
        return EXIT_FAILURE;
    }
    name = argv[1];
    if (strcmp(name, "acquire") == 0) {
        return acquire_main(argc - 2, argv + 2);
    }
    if (strcmp(name, "temperature") == 0) {
        return ost_temperature_main(argc - 2, argv + 2);
    }
    if (strcmp(name, "list") == 0) {
        command = COMMAND_LIST;
    } else if (strcmp(name, "info") == 0) {
        command = COMMAND_INFO;
    } else if (strcmp(name, "spe-info") == 0) {
        command = COMMAND_SPE_INFO;
    } else if (strcmp(name, "spe-dump") == 0) {
        command = COMMAND_SPE_DUMP;
    } else {
        fprintf(stderr, "ost: unknown command: %s\n", name);
        fputs("Usage: ost COMMAND [OPTIONS]\n", stderr);
        return EXIT_FAILURE;
    }
    parsed = parse_options(argc - 2, argv + 2, command, &options);
    if (parsed == 2) {
        return EXIT_SUCCESS;
    }
    if (parsed == 0) {
        command_usage(command, stderr);
        return EXIT_FAILURE;
    }
    switch (command) {
    case COMMAND_LIST:
        result = list_devices(options.json);
        break;
    case COMMAND_INFO:
        result = device_info(options.json, options.camera);
        break;
    case COMMAND_SPE_INFO:
        result = spe_info(options.path, options.json);
        break;
    case COMMAND_SPE_DUMP:
        result = spe_dump(options.path, options.frame, options.json);
        break;
    default:
        return EXIT_FAILURE;
    }
    if (result == EXIT_SUCCESS && fflush(stdout) == EOF) {
        fprintf(stderr, "ost: %s: standard output: I/O error\n", name);
        return EXIT_FAILURE;
    }
    return result;
}
