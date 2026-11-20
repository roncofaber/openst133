/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#include "camera_scan.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define SENSOR_WIDTH 1340u
#define SENSOR_HEIGHT 100u
#define PHYSICAL_LINE (SENSOR_WIDTH + 16u)
#define OP_OPEN 0x40u
#define OP_READ 0x00u
#define OP_BIN_SHIFT 0x20u
#define OP_BIN_READ 0x30u
#define OP_SERIAL_SKIP 0x10u
#define OP_PARALLEL_SHIFT 0x50u
#define OP_REPEAT_BEGIN 0x44u
#define OP_REPEAT_END 0x46u

typedef struct builder {
    ost_camera_scan_program *program;
    int overflow;
} builder;

static void fail(char *error, size_t error_size, const char *format, ...)
{
    va_list arguments;

    if (error == NULL || error_size == 0u) {
        return;
    }
    va_start(arguments, format);
    (void)vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static void emit(builder *output, unsigned char argument)
{
    if (output->program->count == OST_CAMERA_SCAN_MAX_ARGUMENTS) {
        output->overflow = 1;
        return;
    }
    output->program->arguments[output->program->count++] = argument;
}

static void emit_u16(builder *output, uint32_t value)
{
    emit(output, (unsigned char)(value & 0xffu));
    emit(output, (unsigned char)((value >> 8u) & 0xffu));
}

static void emit_literal(builder *output, unsigned char operation, uint32_t count)
{
    uint32_t i;

    for (i = 0u; i < count; ++i) {
        emit(output, operation);
    }
}

static void emit_parallel(builder *output, uint32_t count, int compact)
{
    if (compact && count >= 5u) {
        emit(output, 0x54u);
        emit(output, 0x56u);
        emit_u16(output, count - 2u);
    } else {
        emit_literal(output, OP_PARALLEL_SHIFT, count);
    }
}

static void emit_skips(builder *output, uint32_t count, int compact)
{
    if (compact && count >= 5u) {
        emit(output, 0x14u);
        emit(output, 0x16u);
        emit_u16(output, count - 2u);
    } else {
        emit_literal(output, OP_SERIAL_SKIP, count);
    }
}

static void emit_active_samples(builder *output, uint32_t columns, uint32_t binning)
{
    uint32_t i;

    if (binning == 1u) {
        emit_literal(output, OP_READ, columns);
        return;
    }
    for (i = 0u; i < columns; ++i) {
        emit_literal(output, OP_BIN_SHIFT, binning - 1u);
        emit(output, OP_BIN_READ);
    }
}

static void emit_active_body(builder *output, uint32_t rows_per_sample, uint32_t leading, uint32_t columns,
    uint32_t horizontal_binning, uint32_t trailing, int compact)
{
    emit_parallel(output, rows_per_sample, compact);
    emit_skips(output, leading, compact);
    emit_active_samples(output, columns, horizontal_binning);
    emit_skips(output, trailing, compact);
}

static void emit_discard_pair_block(builder *output, uint32_t repetitions)
{
    emit(output, OP_PARALLEL_SHIFT);
    emit(output, OP_PARALLEL_SHIFT);
    if (repetitions == 1u) {
        emit_skips(output, PHYSICAL_LINE, 1);
    } else {
        emit_skips(output, PHYSICAL_LINE, 0);
    }
}

static void emit_pair_repeat(builder *output, uint32_t repetitions)
{
    if (repetitions == 0u) {
        return;
    }
    if (repetitions == 1u) {
        emit_discard_pair_block(output, repetitions);
        return;
    }
    emit(output, OP_REPEAT_BEGIN);
    emit_discard_pair_block(output, repetitions);
    emit(output, OP_REPEAT_END);
    emit_u16(output, repetitions - 1u);
}

static void emit_residual_rows(builder *output, uint32_t rows)
{
    emit_parallel(output, rows, 1);
    emit_skips(output, PHYSICAL_LINE, 1);
}

static void emit_discarded_rows(builder *output, uint32_t discarded)
{
    uint32_t pairs;
    uint32_t residual;
    uint32_t quantum;

    if (discarded <= 12u) {
        pairs = discarded / 2u;
        residual = discarded % 2u;
        if (residual == 0u) {
            --pairs;
            residual = 2u;
        }
        emit_pair_repeat(output, pairs);
        if (residual != 0u) {
            emit_residual_rows(output, residual);
        }
        return;
    }
    residual = discarded - 10u;
    quantum = 4u;
    while (2u * quantum < residual) {
        residual -= quantum;
        quantum *= 2u;
    }
    if (residual > quantum) {
        emit_residual_rows(output, (residual + 1u) / 2u);
        emit_residual_rows(output, residual / 2u);
    } else {
        emit_residual_rows(output, residual);
    }
    while (quantum > 2u) {
        quantum /= 2u;
        if (quantum > 2u) {
            emit_residual_rows(output, quantum);
        }
    }
    emit_pair_repeat(output, 5u);
}

static int resolve_axis(uint32_t start, uint32_t extent, uint32_t binning, uint32_t sensor_extent,
    uint32_t *samples, uint32_t *effective_end)
{
    uint64_t count = extent / binning;
    uint64_t end;

    if (count == 0u) {
        count = 1u;
    }
    end = (uint64_t)start + count * binning;
    if (end > sensor_extent) {
        return 0;
    }
    *samples = (uint32_t)count;
    *effective_end = (uint32_t)end;
    return 1;
}

int ost_camera_scan_generate(const ost_camera_readout *readout, ost_camera_scan_program *program,
    char *error, size_t error_size)
{
    builder output;
    uint32_t serial_end;
    uint32_t parallel_end;
    uint32_t columns;
    uint32_t rows;
    uint32_t leading;
    uint32_t trailing;

    if (program == NULL) {
        return OST_ERR_INVALID_ARGUMENT;
    }
    memset(program, 0, sizeof(*program));
    if (readout == NULL || readout->width == 0u || readout->height == 0u || readout->x_binning == 0u ||
        readout->y_binning == 0u || readout->x >= SENSOR_WIDTH || readout->y >= SENSOR_HEIGHT ||
        readout->width > SENSOR_WIDTH - readout->x || readout->height > SENSOR_HEIGHT - readout->y ||
        readout->width % readout->x_binning != 0u || readout->height % readout->y_binning != 0u) {
        fail(error, error_size, "ROI and binning must fit the sensor with positive extents and bins");
        return OST_ERR_INVALID_ARGUMENT;
    }
    if (!resolve_axis(readout->x, readout->width, readout->x_binning, SENSOR_WIDTH, &columns, &serial_end)) {
        fail(error, error_size,
            "horizontal binning %u makes the effective command 0x20 scan ROI extend beyond serial position 1340; "
            "the physical-line steps outside the sensor are undetermined", (unsigned int)readout->x_binning);
        return OST_ERR_UNSUPPORTED;
    }
    if (!resolve_axis(readout->y, readout->height, readout->y_binning, SENSOR_HEIGHT, &rows, &parallel_end)) {
        fail(error, error_size,
            "vertical binning %u makes the effective command 0x20 scan ROI extend beyond parallel position 100; "
            "the discarded-row steps outside the sensor are undetermined", (unsigned int)readout->y_binning);
        return OST_ERR_UNSUPPORTED;
    }
    leading = readout->x + 8u;
    trailing = 1348u - serial_end;
    output.program = program;
    output.overflow = 0;
    program->columns = columns;
    program->rows = rows;
    emit(&output, OP_OPEN);
    if (readout->y == 0u) {
        emit_skips(&output, PHYSICAL_LINE, 1);
    } else {
        emit_discarded_rows(&output, readout->y);
    }
    if (rows > 1u) {
        emit(&output, OP_REPEAT_BEGIN);
        emit_active_body(&output, readout->y_binning, leading, columns, readout->x_binning, trailing, 0);
        emit(&output, OP_REPEAT_END);
        emit_u16(&output, rows - 1u);
    } else {
        emit_active_body(&output, readout->y_binning, leading, columns, readout->x_binning, trailing, 1);
    }
    if (parallel_end < SENSOR_HEIGHT) {
        emit_discarded_rows(&output, SENSOR_HEIGHT - parallel_end);
    }
    emit(&output, 0x70u);
    emit(&output, 0x70u);
    if (output.overflow) {
        memset(program, 0, sizeof(*program));
        fail(error, error_size,
            "command 0x20 scan program for the requested ROI and binning exceeds the generator capacity");
        return OST_ERR_UNSUPPORTED;
    }
    return OST_SUCCESS;
}
