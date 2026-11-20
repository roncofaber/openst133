/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#define _FILE_OFFSET_BITS 64

#include "openst133_spe.h"
#include <float.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#endif

struct ost_spe_file {
    unsigned char *bytes;
    size_t file_size;
    ost_spe_info info;
    ost_spe_region *regions;
    uint64_t frame_size;
    uint64_t frame_stride;
};

static uint16_t get16(const unsigned char *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8));
}

static uint32_t get32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint64_t get64(const unsigned char *bytes)
{
    return (uint64_t)get32(bytes) | ((uint64_t)get32(bytes + 4) << 32);
}

static float get_float(const unsigned char *bytes)
{
    uint32_t bits = get32(bytes);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void put16(unsigned char *bytes, uint16_t value)
{
    bytes[0] = (unsigned char)(value & 0xffu);
    bytes[1] = (unsigned char)(value >> 8);
}

static void put32(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)(value & 0xffu);
    bytes[1] = (unsigned char)((value >> 8) & 0xffu);
    bytes[2] = (unsigned char)((value >> 16) & 0xffu);
    bytes[3] = (unsigned char)(value >> 24);
}

static void put64(unsigned char *bytes, uint64_t value)
{
    put32(bytes, (uint32_t)(value & UINT32_MAX));
    put32(bytes + 4u, (uint32_t)(value >> 32));
}

static void put_float(unsigned char *bytes, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    put32(bytes, bits);
}

static FILE *open_exclusive(const char *path)
{
#ifdef _WIN32
    int descriptor = _open(path, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
    FILE *stream;

    if (descriptor < 0) {
        return NULL;
    }
    stream = _fdopen(descriptor, "wb");
    if (stream == NULL) {
        (void)_close(descriptor);
    }
    return stream;
#else
    return fopen(path, "wbx");
#endif
}


static int checked_add(uint64_t a, uint64_t b, uint64_t limit)
{
    return b <= limit && a <= limit - b;
}

static int checked_product(uint64_t a, uint64_t b, uint64_t *product)
{
    if (a != 0u && b > UINT64_MAX / a) {
        return 0;
    }
    *product = a * b;
    return 1;
}

static size_t datatype_size(int data_type)
{
    switch (data_type) {
    case OST_SPE_FLOAT32:
    case OST_SPE_INT32:
    case OST_SPE_UINT32:
        return 4u;
    case OST_SPE_INT16:
    case OST_SPE_UINT16:
        return 2u;
    case OST_SPE_FLOAT64:
        return 8u;
    case OST_SPE_UINT8:
        return 1u;
    default:
        return 0u;
    }
}

static void copy_text(char *destination, size_t capacity, const unsigned char *source)
{
    size_t length = 0u;
    while (length + 1u < capacity && source[length] != 0u) {
        destination[length] = (char)source[length];
        ++length;
    }
    destination[length] = '\0';
}

static int read_entire_file(const char *path, unsigned char **bytes, size_t *file_size)
{
    FILE *stream = fopen(path, "rb");
    unsigned char *contents;
    long end;
    size_t size;

    if (stream == NULL) {
        return OST_SPE_ERR_IO;
    }
    if (fseek(stream, 0L, SEEK_END) != 0) {
        fclose(stream);
        return OST_SPE_ERR_IO;
    }
    end = ftell(stream);
    if (end < (long)OST_SPE_HEADER_SIZE || fseek(stream, 0L, SEEK_SET) != 0) {
        fclose(stream);
        return end < 0L ? OST_SPE_ERR_IO : OST_SPE_ERR_FORMAT;
    }
    size = (size_t)end;
    if ((long)size != end || size == SIZE_MAX) {
        fclose(stream);
        return OST_SPE_ERR_UNSUPPORTED;
    }
    contents = malloc(size + 1u);
    if (contents == NULL) {
        fclose(stream);
        return OST_SPE_ERR_NO_MEMORY;
    }
    if (fread(contents, 1u, size, stream) != size || ferror(stream)) {
        free(contents);
        fclose(stream);
        return OST_SPE_ERR_IO;
    }
    if (fclose(stream) != 0) {
        free(contents);
        return OST_SPE_ERR_IO;
    }
    contents[size] = 0u;
    *bytes = contents;
    *file_size = size;
    return OST_SPE_SUCCESS;
}

static const unsigned char *find_bytes(const unsigned char *start, const unsigned char *end,
                                       const char *literal)
{
    size_t length = strlen(literal);
    const unsigned char *cursor;

    if ((size_t)(end - start) < length) {
        return NULL;
    }
    for (cursor = start; (size_t)(end - cursor) >= length; ++cursor) {
        if (memcmp(cursor, literal, length) == 0) {
            return cursor;
        }
    }
    return NULL;
}

static const unsigned char *find_tag(const unsigned char *start, const unsigned char *end,
                                     const char *name)
{
    char prefix[64];
    size_t name_length;
    const unsigned char *cursor;

    if (strlen(name) + 2u > sizeof(prefix)) {
        return NULL;
    }
    prefix[0] = '<';
    memcpy(prefix + 1u, name, strlen(name) + 1u);
    name_length = strlen(prefix);
    cursor = start;
    for (;;) {
        unsigned char next;
        cursor = find_bytes(cursor, end, prefix);
        if (cursor == NULL) {
            return NULL;
        }
        if ((size_t)(end - cursor) > name_length) {
            next = cursor[name_length];
            if (next == '>' || next == '/' || next == ' ' || next == '\t' ||
                next == '\r' || next == '\n') {
                return cursor;
            }
        }
        cursor += name_length;
    }
}

static int attribute(const unsigned char *tag, const unsigned char *end, const char *name,
                     const unsigned char **value, size_t *value_size)
{
    const unsigned char *tag_end = memchr(tag, '>', (size_t)(end - tag));
    const unsigned char *cursor = tag + 1u;
    size_t name_length = strlen(name);

    if (tag_end == NULL) {
        return OST_SPE_ERR_FORMAT;
    }
    while (cursor < tag_end) {
        const unsigned char *found = find_bytes(cursor, tag_end, name);
        unsigned char before;
        unsigned char after;
        unsigned char quote;
        const unsigned char *start;

        if (found == NULL) {
            return 1;
        }
        before = found == tag ? '<' : *(found - 1u);
        after = found[name_length];
        if (before != ' ' && before != '\t' && before != '\r' && before != '\n') {
            cursor = found + name_length;
            continue;
        }
        cursor = found + name_length;
        while (cursor < tag_end && (*cursor == ' ' || *cursor == '\t' ||
                                    *cursor == '\r' || *cursor == '\n')) {
            ++cursor;
        }
        if (after != '=' && (cursor == tag_end || *cursor != '=')) {
            continue;
        }
        if (*cursor == '=') {
            ++cursor;
        }
        while (cursor < tag_end && (*cursor == ' ' || *cursor == '\t' ||
                                    *cursor == '\r' || *cursor == '\n')) {
            ++cursor;
        }
        if (cursor == tag_end || (*cursor != '"' && *cursor != '\'')) {
            return OST_SPE_ERR_FORMAT;
        }
        quote = *cursor++;
        start = cursor;
        while (cursor < tag_end && *cursor != quote) {
            ++cursor;
        }
        if (cursor == tag_end) {
            return OST_SPE_ERR_FORMAT;
        }
        *value = start;
        *value_size = (size_t)(cursor - start);
        return OST_SPE_SUCCESS;
    }
    return 1;
}

static int required_attribute(const unsigned char *tag, const unsigned char *end, const char *name,
                              const unsigned char **value, size_t *value_size)
{
    int status = attribute(tag, end, name, value, value_size);
    return status == 1 ? OST_SPE_ERR_FORMAT : status;
}

static int attribute_uint64(const unsigned char *tag, const unsigned char *end, const char *name,
                            uint64_t *number)
{
    const unsigned char *value;
    size_t value_size;
    size_t i;
    uint64_t result = 0u;
    int status = required_attribute(tag, end, name, &value, &value_size);

    if (status != OST_SPE_SUCCESS) {
        return status;
    }
    if (value_size == 0u) {
        return OST_SPE_ERR_FORMAT;
    }
    for (i = 0u; i < value_size; ++i) {
        unsigned int digit = (unsigned int)(value[i] - (unsigned char)'0');
        if (digit > 9u || result > (UINT64_MAX - digit) / 10u) {
            return OST_SPE_ERR_FORMAT;
        }
        result = result * 10u + digit;
    }
    *number = result;
    return OST_SPE_SUCCESS;
}

static int attribute_text(const unsigned char *tag, const unsigned char *end, const char *name,
                          char *text, size_t capacity)
{
    const unsigned char *value;
    size_t value_size;
    int status = required_attribute(tag, end, name, &value, &value_size);
    if (status != OST_SPE_SUCCESS) {
        return status;
    }
    if (value_size >= capacity) {
        return OST_SPE_ERR_UNSUPPORTED;
    }
    memcpy(text, value, value_size);
    text[value_size] = '\0';
    return OST_SPE_SUCCESS;
}

static int valid_utf8(const unsigned char *text, size_t size)
{
    size_t i = 0u;
    while (i < size) {
        unsigned int code;
        size_t remaining;
        size_t j;

        if (text[i] == 0u) {
            return 0;
        }
        if (text[i] < 0x80u) {
            ++i;
            continue;
        }
        if (text[i] >= 0xc2u && text[i] <= 0xdfu) {
            code = text[i] & 0x1fu;
            remaining = 1u;
        } else if (text[i] >= 0xe0u && text[i] <= 0xefu) {
            code = text[i] & 0x0fu;
            remaining = 2u;
        } else if (text[i] >= 0xf0u && text[i] <= 0xf4u) {
            code = text[i] & 0x07u;
            remaining = 3u;
        } else {
            return 0;
        }
        if (i + remaining >= size) {
            return 0;
        }
        for (j = 1u; j <= remaining; ++j) {
            if ((text[i + j] & 0xc0u) != 0x80u) {
                return 0;
            }
            code = (code << 6) | (text[i + j] & 0x3fu);
        }
        if ((remaining == 2u && code < 0x800u) || (remaining == 3u && code < 0x10000u) ||
            (code >= 0xd800u && code <= 0xdfffu) || code > 0x10ffffu) {
            return 0;
        }
        i += remaining + 1u;
    }
    return 1;
}

static int parse_spe2(ost_spe_file *file)
{
    uint64_t frame_size;
    uint64_t data_size;
    const unsigned char *header = file->bytes;
    uint32_t xdim = get16(header + 42u);
    uint32_t ydim = get16(header + 656u);
    int32_t frame_count = (int32_t)get32(header + 1446u);

    if (frame_count <= 0 || xdim == 0u || ydim == 0u ||
        !checked_product(xdim, ydim, &file->info.pixel_count) ||
        !checked_product(file->info.pixel_count, file->info.pixel_size, &frame_size)) {
        return OST_SPE_ERR_FORMAT;
    }
    if (!checked_product((uint64_t)frame_count, frame_size, &data_size) ||
        !checked_add(OST_SPE_HEADER_SIZE, data_size, file->file_size) ||
        file->file_size != OST_SPE_HEADER_SIZE + data_size) {
        return OST_SPE_ERR_FORMAT;
    }
    file->regions = malloc(sizeof(*file->regions));
    if (file->regions == NULL) {
        return OST_SPE_ERR_NO_MEMORY;
    }
    file->regions[0].width = xdim;
    file->regions[0].height = ydim;
    file->regions[0].size = frame_size;
    file->regions[0].stride = frame_size;
    file->info.region_count = 1u;
    file->info.width = xdim;
    file->info.height = ydim;
    file->info.frame_count = (uint64_t)frame_count;
    file->frame_size = frame_size;
    file->frame_stride = frame_size;
    return OST_SPE_SUCCESS;
}

static int parse_spe3(ost_spe_file *file)
{
    const unsigned char *xml = file->bytes + OST_SPE_HEADER_SIZE;
    const unsigned char *end = file->bytes + file->file_size;
    const unsigned char *start;
    const unsigned char *data_format;
    const unsigned char *frame;
    const unsigned char *cursor;
    uint64_t footer_offset;
    uint64_t frame_count;
    uint64_t pixel_count = 0u;
    uint64_t region_strides = 0u;
    uint64_t region_count = 0u;
    uint64_t data_extent;
    uint64_t expected_datatype;
    char pixel_format[32];
    char root_version[16];
    size_t index = 0u;

    if (file->file_size < 4164u) {
        return OST_SPE_ERR_FORMAT;
    }
    footer_offset = get64(file->bytes + 678u);
    if (footer_offset < OST_SPE_HEADER_SIZE || footer_offset >= file->file_size) {
        return OST_SPE_ERR_FORMAT;
    }
    xml = file->bytes + (size_t)footer_offset;
    start = xml;
    if ((size_t)(end - start) >= 3u && memcmp(start, "\xef\xbb\xbf", 3u) == 0) {
        start += 3u;
    }
    if (!valid_utf8(start, (size_t)(end - start))) {
        return OST_SPE_ERR_UNSUPPORTED;
    }
    start = find_tag(start, end, "SpeFormat");
    if (start == NULL || attribute_text(start, end, "version", root_version,
                                        sizeof(root_version)) != OST_SPE_SUCCESS ||
        strcmp(root_version, "3.0") != 0) {
        return OST_SPE_ERR_FORMAT;
    }
    data_format = find_tag(start, end, "DataFormat");
    if (data_format == NULL) {
        return OST_SPE_ERR_FORMAT;
    }
    frame = find_tag(data_format, end, "DataBlock");
    if (frame == NULL) {
        return OST_SPE_ERR_FORMAT;
    }
    if (attribute_text(frame, end, "type", pixel_format, sizeof(pixel_format)) != OST_SPE_SUCCESS ||
        strcmp(pixel_format, "Frame") != 0 ||
        attribute_text(frame, end, "pixelFormat", pixel_format, sizeof(pixel_format)) !=
            OST_SPE_SUCCESS ||
        attribute_uint64(frame, end, "count", &frame_count) != OST_SPE_SUCCESS ||
        attribute_uint64(frame, end, "size", &file->frame_size) != OST_SPE_SUCCESS ||
        attribute_uint64(frame, end, "stride", &file->frame_stride) != OST_SPE_SUCCESS ||
        frame_count == 0u || file->frame_size == 0u || file->frame_stride < file->frame_size ||
        !checked_product(frame_count, file->frame_stride, &data_extent) || data_extent > SIZE_MAX ||
        !checked_add(OST_SPE_HEADER_SIZE, data_extent, footer_offset) ||
        (uint64_t)OST_SPE_HEADER_SIZE + data_extent != footer_offset) {
        return OST_SPE_ERR_FORMAT;
    }

    if (strcmp(pixel_format, "MonochromeUnsigned16") == 0) {
        file->info.data_type = OST_SPE_UINT16;
        file->info.pixel_size = 2u;
        expected_datatype = OST_SPE_UINT16;
    } else if (strcmp(pixel_format, "MonochromeUnsigned32") == 0) {
        file->info.data_type = OST_SPE_UINT32;
        file->info.pixel_size = 4u;
        expected_datatype = OST_SPE_UINT32;
    } else if (strcmp(pixel_format, "MonochromeFloating32") == 0) {
        file->info.data_type = OST_SPE_FLOAT32;
        file->info.pixel_size = 4u;
        expected_datatype = OST_SPE_FLOAT32;
    } else {
        return OST_SPE_ERR_UNSUPPORTED;
    }
    if (expected_datatype != (uint16_t)get16(file->bytes + 108u)) {
        return OST_SPE_ERR_FORMAT;
    }

    cursor = frame;
    while ((cursor = find_tag(cursor + 1u, end, "DataBlock")) != NULL) {
        uint64_t ignored;
        if (attribute_uint64(cursor, end, "width", &ignored) == OST_SPE_SUCCESS) {
            ++region_count;
        }
    }
    if (region_count == 0u || region_count > SIZE_MAX / sizeof(*file->regions)) {
        return OST_SPE_ERR_FORMAT;
    }
    file->regions = malloc((size_t)region_count * sizeof(*file->regions));
    if (file->regions == NULL) {
        return OST_SPE_ERR_NO_MEMORY;
    }
    cursor = frame;
    while (index < (size_t)region_count) {
        ost_spe_region *region = &file->regions[index];
        uint64_t width;
        uint64_t height;
        uint64_t count;
        uint64_t size = 0u;
        char type[16];

        cursor = find_tag(cursor + 1u, end, "DataBlock");
        if (cursor == NULL ||
            attribute_text(cursor, end, "type", type, sizeof(type)) != OST_SPE_SUCCESS ||
            strcmp(type, "Region") != 0 ||
            attribute_uint64(cursor, end, "count", &count) != OST_SPE_SUCCESS || count != 1u ||
            attribute_uint64(cursor, end, "width", &width) != OST_SPE_SUCCESS ||
            attribute_uint64(cursor, end, "height", &height) != OST_SPE_SUCCESS ||
            attribute_uint64(cursor, end, "size", &region->size) != OST_SPE_SUCCESS ||
            attribute_uint64(cursor, end, "stride", &region->stride) != OST_SPE_SUCCESS ||
            width == 0u || width > UINT32_MAX || height == 0u || height > UINT32_MAX ||
            region->size == 0u || region->stride < region->size ||
            !checked_product(width, height, &size) ||
            size > UINT64_MAX / file->info.pixel_size ||
            size * file->info.pixel_size != region->size) {
            free(file->regions);
            file->regions = NULL;
            return OST_SPE_ERR_FORMAT;
        }
        region->width = (uint32_t)width;
        region->height = (uint32_t)height;
        region->size = size * file->info.pixel_size;
        if (!checked_add(pixel_count, size, UINT64_MAX) ||
            !checked_add(region_strides, region->stride, file->frame_stride)) {
            free(file->regions);
            file->regions = NULL;
            return OST_SPE_ERR_FORMAT;
        }
        pixel_count += size;
        region_strides += region->stride;
        ++index;
    }
    file->info.pixel_count = pixel_count;
    file->info.frame_count = frame_count;
    file->info.region_count = region_count;
    if (pixel_count > UINT64_MAX / file->info.pixel_size ||
        pixel_count * file->info.pixel_size != file->frame_size ||
        file->frame_size > SIZE_MAX || file->frame_size == 0u) {
        free(file->regions);
        file->regions = NULL;
        return OST_SPE_ERR_FORMAT;
    }
    if (file->info.region_count == 1u) {
        file->info.width = file->regions[0].width;
        file->info.height = file->regions[0].height;
    }
    return OST_SPE_SUCCESS;
}

int ost_spe_read(const char *path, ost_spe_file **result)
{
    ost_spe_file *file;
    int status;

    if (path == NULL || result == NULL) {
        return OST_SPE_ERR_INVALID_ARGUMENT;
    }
    *result = NULL;
    file = calloc(1u, sizeof(*file));
    if (file == NULL) {
        return OST_SPE_ERR_NO_MEMORY;
    }
    status = read_entire_file(path, &file->bytes, &file->file_size);
    if (status != OST_SPE_SUCCESS) {
        free(file);
        return status;
    }
    file->info.header_version = get_float(file->bytes + 1992u);
    if (file->info.header_version >= 2.0f && file->info.header_version < 3.0f) {
        file->info.major_version = 2u;
    } else if (file->info.header_version == 3.0f) {
        file->info.major_version = 3u;
    } else {
        status = OST_SPE_ERR_UNSUPPORTED;
    }
    file->info.data_type = (ost_spe_data_type)(int16_t)get16(file->bytes + 108u);
    file->info.pixel_size = datatype_size(file->info.data_type);
    if (status == OST_SPE_SUCCESS && file->info.pixel_size == 0u) {
        status = OST_SPE_ERR_UNSUPPORTED;
    }
    file->info.exposure_seconds = (double)get_float(file->bytes + 10u);
    file->info.temperature_setpoint = get_float(file->bytes + 36u);
    copy_text(file->info.date, sizeof(file->info.date), file->bytes + 20u);
    copy_text(file->info.software_version, sizeof(file->info.software_version), file->bytes + 688u);
    if (status == OST_SPE_SUCCESS) {
        status = file->info.major_version == 2u ? parse_spe2(file) : parse_spe3(file);
    }
    if (status != OST_SPE_SUCCESS) {
        ost_spe_free(file);
        return status;
    }
    *result = file;
    return OST_SPE_SUCCESS;
}

void ost_spe_free(ost_spe_file *file)
{
    if (file != NULL) {
        free(file->regions);
        free(file->bytes);
        free(file);
    }
}

int ost_spe_get_info(const ost_spe_file *file, ost_spe_info *info)
{
    if (file == NULL || info == NULL) {
        return OST_SPE_ERR_INVALID_ARGUMENT;
    }
    *info = file->info;
    return OST_SPE_SUCCESS;
}

int ost_spe_get_region(const ost_spe_file *file, uint64_t index, ost_spe_region *region)
{
    if (file == NULL || region == NULL) {
        return OST_SPE_ERR_INVALID_ARGUMENT;
    }
    if (index >= file->info.region_count) {
        return OST_SPE_ERR_RANGE;
    }
    *region = file->regions[index];
    return OST_SPE_SUCCESS;
}

int ost_spe_read_frame(const ost_spe_file *file, uint64_t index, void *destination,
                       size_t destination_size, size_t *pixel_bytes)
{
    uint64_t region_offset = OST_SPE_HEADER_SIZE;
    size_t offset = 0u;
    uint64_t i;
    unsigned char *output = destination;

    if (file == NULL || destination == NULL || pixel_bytes == NULL) {
        return OST_SPE_ERR_INVALID_ARGUMENT;
    }
    *pixel_bytes = file->frame_size < (uint64_t)SIZE_MAX ? (size_t)file->frame_size : 0u;
    if (index >= file->info.frame_count) {
        return OST_SPE_ERR_RANGE;
    }
    if (index > (UINT64_MAX - region_offset) / file->frame_stride ||
        destination_size < file->frame_size) {
        return OST_SPE_ERR_INVALID_ARGUMENT;
    }
    region_offset += index * file->frame_stride;
    for (i = 0u; i < file->info.region_count; ++i) {
        if (region_offset > file->file_size || file->regions[i].size > file->file_size - region_offset ||
            file->regions[i].size > SIZE_MAX - offset) {
            return OST_SPE_ERR_FORMAT;
        }
        memcpy(output + offset, file->bytes + region_offset, (size_t)file->regions[i].size);
        offset += (size_t)file->regions[i].size;
        if (!checked_add(region_offset, file->regions[i].stride, file->file_size)) {
            return OST_SPE_ERR_FORMAT;
        }
        region_offset += file->regions[i].stride;
    }
    return OST_SPE_SUCCESS;
}

int ost_spe_header(const ost_spe_file *file, const unsigned char **header, size_t *size)
{
    if (file == NULL || header == NULL || size == NULL) {
        return OST_SPE_ERR_INVALID_ARGUMENT;
    }
    *header = file->bytes;
    *size = OST_SPE_HEADER_SIZE;
    return OST_SPE_SUCCESS;
}

int ost_spe_xml_footer(const ost_spe_file *file, const unsigned char **xml, size_t *size)
{
    uint64_t offset;

    if (file == NULL || xml == NULL || size == NULL || file->info.major_version != 3u) {
        return OST_SPE_ERR_INVALID_ARGUMENT;
    }
    offset = get64(file->bytes + 678u);
    if (offset > SIZE_MAX) {
        return OST_SPE_ERR_UNSUPPORTED;
    }
    *xml = file->bytes + (size_t)offset;
    *size = file->file_size - (size_t)offset;
    return OST_SPE_SUCCESS;
}

#ifndef OPENST133_VERSION
#define OPENST133_VERSION "unknown"
#endif

#define SPE_COMMENT_OFFSET 200u
#define SPE_COMMENT_SIZE 80u
#define SPE_COMMENT_COUNT 5u

static const char spe_months[12][4] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
};

/* SPE ADCrate, ADCtype and ADCresolution values for the supported readout rates. */
static const struct {
    uint32_t rate_hz;
    uint16_t adc_rate;
    uint16_t adc_type;
    uint16_t adc_resolution;
} spe_rate_codes[] = {
    {1000000u, 11u, 8u, 4u},
    {100000u, 6u, 9u, 4u},
};

static int64_t days_from_civil(int64_t year, int month, int day)
{
    int64_t era;
    int64_t year_of_era;
    int64_t day_of_year;
    int64_t day_of_era;

    year -= month <= 2;
    era = (year >= 0 ? year : year - 399) / 400;
    year_of_era = year - era * 400;
    day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + day_of_era - 719468;
}

static void put_hhmmss(unsigned char *bytes, const struct tm *time)
{
    bytes[0] = (unsigned char)('0' + time->tm_hour / 10);
    bytes[1] = (unsigned char)('0' + time->tm_hour % 10);
    bytes[2] = (unsigned char)('0' + time->tm_min / 10);
    bytes[3] = (unsigned char)('0' + time->tm_min % 10);
    bytes[4] = (unsigned char)('0' + time->tm_sec / 10);
    bytes[5] = (unsigned char)('0' + time->tm_sec % 10);
}

static int valid_tm(const struct tm *time)
{
    return time->tm_mon >= 0 && time->tm_mon < 12 && time->tm_mday >= 1 && time->tm_mday <= 31 &&
           time->tm_year >= -1900 && time->tm_year <= 8099 && time->tm_hour >= 0 && time->tm_hour < 24 &&
           time->tm_min >= 0 && time->tm_min < 60 && time->tm_sec >= 0 && time->tm_sec <= 60;
}

/* Date as DDMonYYYY in local time at 20, local hhmmss at 172, UTC hhmmss at 179; iso gets the UTC W3C time. */
static int set_acquisition_time(unsigned char *header, int64_t acquired_utc, char *iso, size_t iso_size)
{
    time_t when = (time_t)acquired_utc;
    struct tm local;
    struct tm utc;
    const struct tm *converted;
    int year;

    if ((int64_t)when != acquired_utc) {
        return 0;
    }
    converted = localtime(&when);
    if (converted == NULL) {
        return 0;
    }
    local = *converted;
    converted = gmtime(&when);
    if (converted == NULL) {
        return 0;
    }
    utc = *converted;
    if (!valid_tm(&local) || !valid_tm(&utc)) {
        return 0;
    }
    year = local.tm_year + 1900;
    header[20u] = (unsigned char)('0' + local.tm_mday / 10);
    header[21u] = (unsigned char)('0' + local.tm_mday % 10);
    memcpy(header + 22u, spe_months[local.tm_mon], 3u);
    header[25u] = (unsigned char)('0' + year / 1000);
    header[26u] = (unsigned char)('0' + year / 100 % 10);
    header[27u] = (unsigned char)('0' + year / 10 % 10);
    header[28u] = (unsigned char)('0' + year % 10);
    put_hhmmss(header + 172u, &local);
    put_hhmmss(header + 179u, &utc);
    return snprintf(iso, iso_size, "%04d-%02d-%02dT%02d:%02d:%02dZ", utc.tm_year + 1900, utc.tm_mon + 1,
                    utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec) == 20;
}

static int parse_digits(const char *text, size_t count, int *value)
{
    size_t index;

    *value = 0;
    for (index = 0u; index < count; ++index) {
        if (text[index] < '0' || text[index] > '9') {
            return 0;
        }
        *value = *value * 10 + (text[index] - '0');
    }
    return 1;
}

static int parse_hhmmss(const char *text, int *seconds)
{
    int hour;
    int minute;
    int second;

    if (strlen(text) != 6u || !parse_digits(text, 2u, &hour) || !parse_digits(text + 2u, 2u, &minute) ||
        !parse_digits(text + 4u, 2u, &second) || hour > 23 || minute > 59 || second > 60) {
        return 0;
    }
    *seconds = hour * 3600 + minute * 60 + second;
    return 1;
}

/* Local date and time give local seconds; the UTC time of day fixes the zone offset to within a day. */
static int64_t acquisition_utc(const char *date, const char *time_local, const char *time_utc)
{
    int day;
    int year;
    int month;
    int local_seconds;
    int utc_seconds;
    int offset;

    if (strlen(date) != 9u || !parse_digits(date, 2u, &day) || !parse_digits(date + 5u, 4u, &year) ||
        !parse_hhmmss(time_local, &local_seconds) || !parse_hhmmss(time_utc, &utc_seconds)) {
        return 0;
    }
    for (month = 0; month < 12; ++month) {
        if (memcmp(date + 2u, spe_months[month], 3u) == 0) {
            break;
        }
    }
    if (month == 12 || day < 1 || day > 31) {
        return 0;
    }
    offset = local_seconds - utc_seconds;
    if (offset <= -12 * 3600) {
        offset += 24 * 3600;
    } else if (offset > 14 * 3600) {
        offset -= 24 * 3600;
    }
    return days_from_civil(year, month + 1, day) * 86400 + local_seconds - offset;
}

static int printable_text(const char *text, size_t capacity)
{
    size_t index;

    for (index = 0u; index < capacity && text[index] != '\0'; ++index) {
        if ((unsigned char)text[index] < 0x20u || (unsigned char)text[index] > 0x7eu ||
            text[index] == ';' || text[index] == '=') {
            return 0;
        }
    }
    return index < capacity;
}

static int finite_float(double value)
{
    return value == value && value >= -FLT_MAX && value <= FLT_MAX;
}

static int valid_region(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t x_binning,
    uint32_t y_binning)
{
    return width != 0u && height != 0u && x_binning != 0u && y_binning != 0u && x < UINT16_MAX &&
        y < UINT16_MAX && width <= UINT16_MAX - x && height <= UINT16_MAX - y &&
        x_binning <= UINT16_MAX && y_binning <= UINT16_MAX;
}

static int valid_metadata(const ost_spe_metadata *metadata, uint32_t width, uint32_t height)
{
    if (!finite_float(metadata->exposure_seconds) || metadata->exposure_seconds < 0.0 ||
        metadata->accumulations > INT32_MAX || metadata->controller_gain > UINT16_MAX ||
        metadata->detector_id > INT16_MAX || metadata->chip_width > UINT16_MAX ||
        metadata->chip_height > UINT16_MAX ||
        (metadata->temperature_known && !finite_float(metadata->temperature_celsius)) ||
        (metadata->setpoint_known && !finite_float(metadata->setpoint_celsius)) ||
        (metadata->requested_exposure_known &&
         (!finite_float(metadata->requested_exposure_seconds) || metadata->requested_exposure_seconds < 0.0)) ||
        (metadata->lock_tolerance_known &&
         (!finite_float(metadata->lock_tolerance_celsius) || metadata->lock_tolerance_celsius < 0.0)) ||
        (metadata->lock_hold_known &&
         (!finite_float(metadata->lock_hold_seconds) || metadata->lock_hold_seconds < 0.0)) ||
        (metadata->lock_settle_known &&
         (!finite_float(metadata->lock_settle_seconds) || metadata->lock_settle_seconds < 0.0)) ||
        (metadata->shutter != OST_SPE_SHUTTER_UNKNOWN && metadata->shutter != OST_SPE_SHUTTER_NORMAL &&
         metadata->shutter != OST_SPE_SHUTTER_CLOSED && metadata->shutter != OST_SPE_SHUTTER_OPENED) ||
        !printable_text(metadata->detector_name, sizeof(metadata->detector_name)) ||
        !printable_text(metadata->target_source, sizeof(metadata->target_source)) ||
        !printable_text(metadata->calibration_status, sizeof(metadata->calibration_status))) {
        return 0;
    }
    if (metadata->requested_roi_known &&
        !valid_region(metadata->requested_roi_x, metadata->requested_roi_y, metadata->requested_roi_width,
            metadata->requested_roi_height, metadata->requested_x_binning, metadata->requested_y_binning)) {
        return 0;
    }
    if (metadata->roi_known) {
        uint32_t output_width;
        uint32_t output_height;
        if (!valid_region(metadata->roi_x, metadata->roi_y, metadata->roi_width, metadata->roi_height,
                metadata->x_binning, metadata->y_binning)) {
            return 0;
        }
        output_width = metadata->roi_width / metadata->x_binning;
        output_height = metadata->roi_height / metadata->y_binning;
        if ((output_width == 0u ? 1u : output_width) != width ||
            (output_height == 0u ? 1u : output_height) != height) {
            return 0;
        }
    }
    return 1;
}

typedef struct {
    char *text;
    size_t capacity;
    size_t length;
    int overflow;
} xml_buffer;

static void xml_append(xml_buffer *buffer, const char *format, ...)
{
    va_list arguments;
    int written;

    if (buffer->overflow) {
        return;
    }
    va_start(arguments, format);
    written = vsnprintf(buffer->text + buffer->length, buffer->capacity - buffer->length, format, arguments);
    va_end(arguments);
    if (written < 0 || (size_t)written >= buffer->capacity - buffer->length) {
        buffer->overflow = 1;
        return;
    }
    buffer->length += (size_t)written;
}

static void xml_append_escaped(xml_buffer *buffer, const char *text)
{
    for (; *text != '\0'; ++text) {
        switch (*text) {
        case '&':
            xml_append(buffer, "&amp;");
            break;
        case '<':
            xml_append(buffer, "&lt;");
            break;
        case '>':
            xml_append(buffer, "&gt;");
            break;
        case '"':
            xml_append(buffer, "&quot;");
            break;
        default:
            xml_append(buffer, "%c", *text);
            break;
        }
    }
}

static const char *shutter_name(ost_spe_shutter shutter)
{
    return shutter == OST_SPE_SHUTTER_NORMAL ? "normal" : shutter == OST_SPE_SHUTTER_CLOSED ? "closed"
        : shutter == OST_SPE_SHUTTER_OPENED ? "opened" : NULL;
}

static void xml_double_attribute(xml_buffer *xml, const char *name, int known, double value)
{
    xml_append(xml, " %s=\"", name);
    if (known) {
        xml_append(xml, "%.17g", value);
    } else {
        xml_append(xml, "unknown");
    }
    xml_append(xml, "\"");
}

static void xml_u64_attribute(xml_buffer *xml, const char *name, int known, uint64_t value)
{
    xml_append(xml, " %s=\"", name);
    if (known) {
        xml_append(xml, "%llu", (unsigned long long)value);
    } else {
        xml_append(xml, "unknown");
    }
    xml_append(xml, "\"");
}

static void xml_text_attribute(xml_buffer *xml, const char *name, const char *value)
{
    xml_append(xml, " %s=\"", name);
    xml_append_escaped(xml, value[0] != '\0' ? value : "unknown");
    xml_append(xml, "\"");
}

static void xml_provenance_region(xml_buffer *xml, const char *tag, int known, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height, uint32_t x_binning, uint32_t y_binning)
{
    xml_append(xml, "<%s", tag);
    xml_u64_attribute(xml, "x", known, x);
    xml_u64_attribute(xml, "y", known, y);
    xml_u64_attribute(xml, "width", known, width);
    xml_u64_attribute(xml, "height", known, height);
    xml_u64_attribute(xml, "xBinning", known, x_binning);
    xml_u64_attribute(xml, "yBinning", known, y_binning);
    xml_append(xml, "/>");
}

static const char *generated_value_rule(const ost_spe_metadata *metadata)
{
    return metadata->requested_exposure_known ? "floor" : "unknown";
}

static void build_provenance_xml(xml_buffer *xml, const ost_spe_metadata *metadata)
{
    xml_append(xml, "<Provenance xmlns=\"urn:openst133:provenance:1\"><Exposure");
    xml_double_attribute(xml, "requestedSeconds", metadata->requested_exposure_known,
        metadata->requested_exposure_seconds);
    xml_double_attribute(xml, "effectiveSeconds", 1, metadata->exposure_seconds);
    xml_text_attribute(xml, "generatedValueRule", generated_value_rule(metadata));
    xml_append(xml, "/>");
    xml_provenance_region(xml, "RequestedRegion", metadata->requested_roi_known,
        metadata->requested_roi_x, metadata->requested_roi_y, metadata->requested_roi_width,
        metadata->requested_roi_height, metadata->requested_x_binning, metadata->requested_y_binning);
    xml_provenance_region(xml, "EffectiveRegion", metadata->roi_known, metadata->roi_x, metadata->roi_y,
        metadata->roi_width, metadata->roi_height, metadata->x_binning, metadata->y_binning);
    xml_append(xml, "<Selection");
    xml_text_attribute(xml, "targetSource", metadata->target_source);
    xml_append(xml, "/><SoftwareLock");
    xml_double_attribute(xml, "toleranceCelsius", metadata->lock_tolerance_known,
        metadata->lock_tolerance_celsius);
    xml_double_attribute(xml, "holdSeconds", metadata->lock_hold_known, metadata->lock_hold_seconds);
    xml_double_attribute(xml, "settleSeconds", metadata->lock_settle_known, metadata->lock_settle_seconds);
    xml_append(xml, "/><Stream");
    xml_u64_attribute(xml, "framesDelivered", metadata->stream_known, metadata->stream_frames_delivered);
    xml_u64_attribute(xml, "framesLagged", metadata->stream_known, metadata->stream_frames_lagged);
    xml_u64_attribute(xml, "hostStalls", metadata->stream_known, metadata->stream_host_stalls);
    xml_u64_attribute(xml, "maxReady", metadata->stream_known, metadata->stream_max_ready);
    xml_u64_attribute(xml, "shortTransfers", metadata->stream_known, metadata->stream_short_transfers);
    xml_u64_attribute(xml, "maxCallbackUs", metadata->stream_known, metadata->stream_max_callback_us);
    xml_u64_attribute(xml, "bytesReceived", metadata->stream_known, metadata->stream_bytes_received);
    xml_u64_attribute(xml, "transfersCompleted", metadata->stream_known, metadata->stream_transfers_completed);
    xml_u64_attribute(xml, "partialBytesDiscarded", metadata->stream_known,
        metadata->stream_partial_bytes_discarded);
    xml_append(xml, " stopped=\"");
    if (metadata->stream_known) {
        xml_append(xml, metadata->stream_stopped ? "true" : "false");
    } else {
        xml_append(xml, "unknown");
    }
    xml_append(xml, "\"/><Calibration status=\"");
    xml_append_escaped(xml,
        metadata->calibration_status[0] != '\0' ? metadata->calibration_status : "uncalibrated");
    xml_append(xml, "\"/></Provenance>");
}

static void put_comment(unsigned char *header, unsigned int slot, const char *format, ...)
{
    char text[SPE_COMMENT_SIZE];
    va_list arguments;

    va_start(arguments, format);
    (void)vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    memcpy(header + SPE_COMMENT_OFFSET + slot * SPE_COMMENT_SIZE, text, strlen(text));
}

static void build_metadata_xml(xml_buffer *xml, const ost_spe_metadata *metadata, uint64_t frame_count,
                               const char *created)
{
    const char *shutter = shutter_name(metadata->shutter);

    xml_append(xml, "<DataHistories><DataHistory><Origin created=\"%s\" software=\"openst133\" "
               "softwareVersion=\"%s\"><Acquisition xmlns=\"urn:openst133:acquisition:1\" "
               "exposureSeconds=\"%.9g\" frames=\"%llu\"", created, OPENST133_VERSION,
               (double)(float)metadata->exposure_seconds, (unsigned long long)frame_count);
    if (metadata->accumulations != 0u) {
        xml_append(xml, " accumulations=\"%lu\"", (unsigned long)metadata->accumulations);
    }
    if (metadata->readout_rate_hz != 0u) {
        xml_append(xml, " readoutRateHz=\"%lu\"", (unsigned long)metadata->readout_rate_hz);
    }
    if (metadata->controller_gain != 0u) {
        xml_append(xml, " controllerGain=\"%lu\"", (unsigned long)metadata->controller_gain);
    }
    if (shutter != NULL) {
        xml_append(xml, " shutter=\"%s\"", shutter);
    }
    xml_append(xml, ">");
    if (metadata->detector_id != 0u || metadata->detector_name[0] != '\0' || metadata->chip_width != 0u ||
        metadata->chip_height != 0u) {
        xml_append(xml, "<Detector");
        if (metadata->detector_id != 0u) {
            xml_append(xml, " id=\"%lu\"", (unsigned long)metadata->detector_id);
        }
        if (metadata->detector_name[0] != '\0') {
            xml_append(xml, " name=\"");
            xml_append_escaped(xml, metadata->detector_name);
            xml_append(xml, "\"");
        }
        if (metadata->chip_width != 0u) {
            xml_append(xml, " width=\"%lu\"", (unsigned long)metadata->chip_width);
        }
        if (metadata->chip_height != 0u) {
            xml_append(xml, " height=\"%lu\"", (unsigned long)metadata->chip_height);
        }
        xml_append(xml, "/>");
    }
    if (metadata->roi_known) {
        xml_append(xml, "<Region x=\"%lu\" y=\"%lu\" width=\"%lu\" height=\"%lu\" xBinning=\"%lu\" "
                   "yBinning=\"%lu\"/>", (unsigned long)metadata->roi_x, (unsigned long)metadata->roi_y,
                   (unsigned long)metadata->roi_width, (unsigned long)metadata->roi_height,
                   (unsigned long)metadata->x_binning, (unsigned long)metadata->y_binning);
    }
    if (metadata->temperature_known || metadata->setpoint_known) {
        xml_append(xml, "<Temperature");
        if (metadata->temperature_known) {
            xml_append(xml, " measuredCelsius=\"%.2f\"", metadata->temperature_celsius);
        }
        if (metadata->setpoint_known) {
            xml_append(xml, " setpointCelsius=\"%.9g\"", (double)(float)metadata->setpoint_celsius);
        }
        xml_append(xml, "/>");
    }
    xml_append(xml, "</Acquisition>");
    build_provenance_xml(xml, metadata);
    xml_append(xml, "</Origin></DataHistory></DataHistories>"
               "<GeneralInformation><FileInformation created=\"%s\" lastModified=\"%s\"/>"
               "</GeneralInformation>", created, created);
}

static void put_metadata(unsigned char *header, const ost_spe_metadata *metadata)
{
    char software[17];
    const char *shutter = shutter_name(metadata->shutter);
    size_t index;

    put_float(header + 10u, (float)metadata->exposure_seconds);
    (void)snprintf(software, sizeof(software), "openst133 %s", OPENST133_VERSION);
    memcpy(header + 688u, software, strlen(software));
    if (metadata->setpoint_known) {
        put_float(header + 36u, (float)metadata->setpoint_celsius);
    }
    put16(header + 6u, (uint16_t)metadata->chip_width);
    put16(header + 18u, (uint16_t)metadata->chip_height);
    put16(header + 40u, (uint16_t)metadata->detector_id);
    put16(header + 50u, (uint16_t)metadata->shutter);
    put16(header + 198u, (uint16_t)metadata->controller_gain);
    put32(header + 668u, metadata->accumulations);
    for (index = 0u; index < sizeof(spe_rate_codes) / sizeof(spe_rate_codes[0]); ++index) {
        if (spe_rate_codes[index].rate_hz == metadata->readout_rate_hz) {
            put16(header + 190u, spe_rate_codes[index].adc_rate);
            put16(header + 192u, spe_rate_codes[index].adc_type);
            put16(header + 194u, spe_rate_codes[index].adc_resolution);
        }
    }
    if (metadata->roi_known) {
        put16(header + 1488u, 1u);
        put16(header + 1510u, 1u);
        put16(header + 1512u, (uint16_t)(metadata->roi_x + 1u));
        put16(header + 1514u, (uint16_t)(metadata->roi_x + metadata->roi_width));
        put16(header + 1516u, (uint16_t)metadata->x_binning);
        put16(header + 1518u, (uint16_t)(metadata->roi_y + 1u));
        put16(header + 1520u, (uint16_t)(metadata->roi_y + metadata->roi_height));
        put16(header + 1522u, (uint16_t)metadata->y_binning);
    }
    if (metadata->detector_name[0] != '\0') {
        put_comment(header, 0u, "detector=%s", metadata->detector_name);
    }
    if (metadata->temperature_known) {
        put_comment(header, 1u, "temperature_c=%.2f", metadata->temperature_celsius);
    }
    if (shutter != NULL) {
        put_comment(header, 2u, "shutter=%s", shutter);
    }
    if (metadata->readout_rate_hz != 0u) {
        put_comment(header, 3u, "readout_rate_hz=%lu", (unsigned long)metadata->readout_rate_hz);
    }
}


static int write_native_frames(FILE *stream, const void *frames, size_t pixel_count,
                               ost_spe_data_type data_type)
{
    const unsigned char *source = frames;
    size_t pixel_size = datatype_size(data_type);
    size_t index;

    for (index = 0u; index < pixel_count; ++index) {
        unsigned char output[4];
        if (data_type == OST_SPE_UINT16) {
            uint16_t value;
            memcpy(&value, source, sizeof(value));
            put16(output, value);
        } else {
            uint32_t value;
            memcpy(&value, source, sizeof(value));
            put32(output, value);
        }
        if (fwrite(output, 1u, pixel_size, stream) != pixel_size) {
            return 0;
        }
        source += pixel_size;
    }
    return 1;
}

void ost_spe_metadata_init(ost_spe_metadata *metadata)
{
    if (metadata != NULL) {
        memset(metadata, 0, sizeof(*metadata));
    }
}

int ost_spe_create(const char *path, uint32_t width, uint32_t height,
                   ost_spe_data_type data_type, uint64_t frame_count,
                   const void *frames, size_t frames_size, double exposure_seconds,
                   const float *temperature_setpoint)
{
    ost_spe_metadata metadata;

    ost_spe_metadata_init(&metadata);
    metadata.exposure_seconds = exposure_seconds;
    if (temperature_setpoint != NULL) {
        metadata.setpoint_known = 1;
        metadata.setpoint_celsius = *temperature_setpoint;
    }
    return ost_spe_create_with_metadata(path, width, height, data_type, frame_count, frames, frames_size,
                                        &metadata);
}

int ost_spe_create_with_metadata(const char *path, uint32_t width, uint32_t height,
                                 ost_spe_data_type data_type, uint64_t frame_count,
                                 const void *frames, size_t frames_size,
                                 const ost_spe_metadata *metadata)
{
    unsigned char header[OST_SPE_HEADER_SIZE] = {0};
    const char *pixel_format;
    uint64_t pixel_count;
    uint64_t frame_size;
    uint64_t data_size;
    uint64_t footer_offset;
    int64_t acquired_utc;
    FILE *stream;
    char text[8192];
    char created[32];
    xml_buffer xml = {text, sizeof(text), 0u, 0};
    int result = OST_SPE_SUCCESS;

    if (path == NULL || frames == NULL || metadata == NULL || width == 0u || height == 0u ||
        frame_count == 0u) {
        return OST_SPE_ERR_INVALID_ARGUMENT;
    }
    if (width > UINT16_MAX || height > UINT16_MAX || frame_count > INT32_MAX) {
        return OST_SPE_ERR_RANGE;
    }
    if (data_type == OST_SPE_UINT16) {
        pixel_format = "MonochromeUnsigned16";
    } else if (data_type == OST_SPE_FLOAT32) {
        pixel_format = "MonochromeFloating32";
    } else if (data_type == OST_SPE_INT32) {
        /* SPE 3.0 has no signed 32-bit pixel format: write WinSpec's LONG file, a 2.2 header without footer. */
        pixel_format = NULL;
    } else {
        return OST_SPE_ERR_UNSUPPORTED;
    }
    if (!valid_metadata(metadata, width, height)) {
        return OST_SPE_ERR_INVALID_ARGUMENT;
    }
    if (!checked_product(width, height, &pixel_count) ||
        !checked_product(pixel_count, datatype_size(data_type), &frame_size) ||
        !checked_product(frame_count, frame_size, &data_size) || data_size > SIZE_MAX ||
        (size_t)data_size != frames_size) {
        return OST_SPE_ERR_RANGE;
    }
    acquired_utc = metadata->acquired_utc != 0 ? metadata->acquired_utc : (int64_t)time(NULL);
    if (!set_acquisition_time(header, acquired_utc, created, sizeof(created))) {
        return OST_SPE_ERR_RANGE;
    }
    footer_offset = pixel_format != NULL ? (uint64_t)OST_SPE_HEADER_SIZE + data_size : 0u;
    xml_append(&xml, "<SpeFormat version=\"3.0\" "
               "xmlns=\"http://www.princetoninstruments.com/spe/2009\">"
               "<DataFormat><DataBlock type=\"Frame\" count=\"%llu\" "
               "pixelFormat=\"%s\" size=\"%llu\" stride=\"%llu\">"
               "<DataBlock type=\"Region\" count=\"1\" width=\"%u\" height=\"%u\" "
               "size=\"%llu\" stride=\"%llu\"/>"
               "</DataBlock></DataFormat>",
               (unsigned long long)frame_count, pixel_format != NULL ? pixel_format : "",
               (unsigned long long)frame_size, (unsigned long long)frame_size,
               width, height, (unsigned long long)frame_size,
               (unsigned long long)frame_size);
    build_metadata_xml(&xml, metadata, frame_count, created);
    xml_append(&xml, "</SpeFormat>");
    if (xml.overflow) {
        return OST_SPE_ERR_UNSUPPORTED;
    }
    if (pixel_format == NULL) {
        xml.length = 0u;
    }

    put_metadata(header, metadata);
    if (metadata->chip_width == 0u) {
        put16(header + 6u, (uint16_t)width);
    }
    if (metadata->chip_height == 0u) {
        put16(header + 18u, (uint16_t)height);
    }
    put_float(header + 1992u, pixel_format != NULL ? 3.0f : 2.2f);
    put64(header + 678u, footer_offset);
    put16(header + 108u, (uint16_t)data_type);
    put16(header + 42u, (uint16_t)width);
    put16(header + 656u, (uint16_t)height);
    put32(header + 1446u, (uint32_t)frame_count);
    put16(header + 34u, UINT16_MAX);
    put32(header + 664u, UINT32_MAX);
    put16(header + 658u, 1u);
    put32(header + 2996u, 0x01234567u);
    put16(header + 4098u, 0x5555u);

    stream = open_exclusive(path);
    if (stream == NULL) {
        return OST_SPE_ERR_IO;
    }
    if (fwrite(header, 1u, sizeof(header), stream) != sizeof(header) ||
        !write_native_frames(stream, frames, (size_t)pixel_count * (size_t)frame_count, data_type) ||
        fwrite(text, 1u, xml.length, stream) != xml.length) {
        result = OST_SPE_ERR_IO;
    }
    if (fclose(stream) != 0) {
        result = OST_SPE_ERR_IO;
    }
    return result;
}

int ost_spe_write(const ost_spe_file *file, const char *path)
{
    FILE *stream;
    int result = OST_SPE_SUCCESS;

    if (file == NULL || path == NULL) {
        return OST_SPE_ERR_INVALID_ARGUMENT;
    }
    stream = open_exclusive(path);
    if (stream == NULL) {
        return OST_SPE_ERR_IO;
    }
    if (fwrite(file->bytes, 1u, file->file_size, stream) != file->file_size) {
        result = OST_SPE_ERR_IO;
    }
    if (fclose(stream) != 0) {
        result = OST_SPE_ERR_IO;
    }
    return result;
}

static void comment_value(const unsigned char *header, const char *key, char *value, size_t capacity)
{
    size_t key_length = strlen(key);
    unsigned int slot;

    value[0] = '\0';
    for (slot = 0u; slot < SPE_COMMENT_COUNT; ++slot) {
        char comment[SPE_COMMENT_SIZE + 1u];
        memcpy(comment, header + SPE_COMMENT_OFFSET + slot * SPE_COMMENT_SIZE, SPE_COMMENT_SIZE);
        comment[SPE_COMMENT_SIZE] = '\0';
        if (strncmp(comment, key, key_length) == 0 && comment[key_length] == '=') {
            copy_text(value, capacity, (const unsigned char *)comment + key_length + 1u);
            return;
        }
    }
}

int ost_spe_get_metadata(const ost_spe_file *file, ost_spe_metadata *metadata)
{
    const unsigned char *header;
    char value[SPE_COMMENT_SIZE + 1u];
    char *end;
    uint16_t shutter;
    uint16_t adc_rate;
    size_t index;

    if (file == NULL || metadata == NULL) {
        return OST_SPE_ERR_INVALID_ARGUMENT;
    }
    header = file->bytes;
    ost_spe_metadata_init(metadata);
    metadata->exposure_seconds = (double)get_float(header + 10u);
    metadata->accumulations = (int32_t)get32(header + 668u) > 0 ? get32(header + 668u) : 0u;
    if ((int16_t)get16(header + 1510u) >= 1 && get16(header + 1512u) >= 1u &&
        get16(header + 1514u) >= get16(header + 1512u) && get16(header + 1518u) >= 1u &&
        get16(header + 1520u) >= get16(header + 1518u) && get16(header + 1516u) >= 1u &&
        get16(header + 1522u) >= 1u) {
        metadata->roi_known = 1;
        metadata->roi_x = get16(header + 1512u) - 1u;
        metadata->roi_width = (uint32_t)get16(header + 1514u) - get16(header + 1512u) + 1u;
        metadata->x_binning = get16(header + 1516u);
        metadata->roi_y = get16(header + 1518u) - 1u;
        metadata->roi_height = (uint32_t)get16(header + 1520u) - get16(header + 1518u) + 1u;
        metadata->y_binning = get16(header + 1522u);
    }
    adc_rate = get16(header + 190u);
    for (index = 0u; index < sizeof(spe_rate_codes) / sizeof(spe_rate_codes[0]); ++index) {
        if (adc_rate == spe_rate_codes[index].adc_rate && get16(header + 192u) == spe_rate_codes[index].adc_type) {
            metadata->readout_rate_hz = spe_rate_codes[index].rate_hz;
        }
    }
    comment_value(header, "readout_rate_hz", value, sizeof(value));
    if (value[0] != '\0') {
        unsigned long rate = strtoul(value, &end, 10);
        if (*end == '\0' && rate > 0ul && rate <= UINT32_MAX) {
            metadata->readout_rate_hz = (uint32_t)rate;
        }
    }
    metadata->controller_gain = get16(header + 198u);
    metadata->detector_id = (int16_t)get16(header + 40u) > 0 ? get16(header + 40u) : 0u;
    comment_value(header, "detector", metadata->detector_name, sizeof(metadata->detector_name));
    metadata->chip_width = get16(header + 6u);
    metadata->chip_height = get16(header + 18u);
    comment_value(header, "temperature_c", value, sizeof(value));
    if (value[0] != '\0') {
        double celsius = strtod(value, &end);
        if (*end == '\0' && finite_float(celsius)) {
            metadata->temperature_known = 1;
            metadata->temperature_celsius = celsius;
        }
    }
    metadata->setpoint_celsius = (double)get_float(header + 36u);
    metadata->setpoint_known = metadata->setpoint_celsius != 0.0 && finite_float(metadata->setpoint_celsius);
    if (!metadata->setpoint_known) {
        metadata->setpoint_celsius = 0.0;
    }
    shutter = get16(header + 50u);
    if (shutter == OST_SPE_SHUTTER_NORMAL || shutter == OST_SPE_SHUTTER_CLOSED || shutter == OST_SPE_SHUTTER_OPENED) {
        metadata->shutter = (ost_spe_shutter)shutter;
    }
    comment_value(header, "shutter", value, sizeof(value));
    if (strcmp(value, "normal") == 0) {
        metadata->shutter = OST_SPE_SHUTTER_NORMAL;
    } else if (strcmp(value, "closed") == 0) {
        metadata->shutter = OST_SPE_SHUTTER_CLOSED;
    } else if (strcmp(value, "opened") == 0) {
        metadata->shutter = OST_SPE_SHUTTER_OPENED;
    }
    copy_text(metadata->date, sizeof(metadata->date), header + 20u);
    copy_text(metadata->time_local, sizeof(metadata->time_local), header + 172u);
    copy_text(metadata->time_utc, sizeof(metadata->time_utc), header + 179u);
    copy_text(metadata->software_version, sizeof(metadata->software_version), header + 688u);
    metadata->acquired_utc = acquisition_utc(metadata->date, metadata->time_local, metadata->time_utc);
    return OST_SPE_SUCCESS;
}
