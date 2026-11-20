/*
 * Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
 * SPDX-License-Identifier: MIT
 */

#define _POSIX_C_SOURCE 200809L

#include "openst133_spe.h"
#include "test_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put16(unsigned char *buffer, size_t offset, uint16_t value)
{
    buffer[offset] = (unsigned char)(value & 0xffu);
    buffer[offset + 1] = (unsigned char)(value >> 8);
}

static void put32(unsigned char *buffer, size_t offset, uint32_t value)
{
    size_t i;
    for (i = 0; i < 4u; ++i) {
        buffer[offset + i] = (unsigned char)(value >> (i * 8u));
    }
}

static void put64(unsigned char *buffer, size_t offset, uint64_t value)
{
    size_t i;
    for (i = 0; i < 8u; ++i) {
        buffer[offset + i] = (unsigned char)(value >> (i * 8u));
    }
}

static void put_float(unsigned char *buffer, size_t offset, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    put32(buffer, offset, bits);
}

static void write_file(const char *path, const void *bytes, size_t size)
{
    FILE *stream = fopen(path, "wb");
    TEST_CHECK(stream != NULL);
    TEST_CHECK(fwrite(bytes, 1u, size, stream) == size);
    TEST_CHECK(fclose(stream) == 0);
}

static void compare_files(const char *left_path, const char *right_path)
{
    unsigned char left[8192];
    unsigned char right[8192];
    FILE *left_file = fopen(left_path, "rb");
    FILE *right_file = fopen(right_path, "rb");
    size_t left_size;
    size_t right_size;

    TEST_CHECK(left_file != NULL);
    TEST_CHECK(right_file != NULL);
    left_size = fread(left, 1u, sizeof(left), left_file);
    right_size = fread(right, 1u, sizeof(right), right_file);
    TEST_CHECK(!ferror(left_file));
    TEST_CHECK(!ferror(right_file));
    TEST_CHECK(feof(left_file));
    TEST_CHECK(feof(right_file));
    TEST_CHECK(fclose(left_file) == 0);
    TEST_CHECK(fclose(right_file) == 0);
    TEST_CHECK(left_size == right_size);
    TEST_CHECK(memcmp(left, right, left_size) == 0);
}

static void check_round_trip(const char *source, const char *destination)
{
    ost_spe_file *file = NULL;
    ost_spe_info info;

    TEST_CHECK(ost_spe_read(source, &file) == OST_SPE_SUCCESS);
    TEST_CHECK(ost_spe_get_info(file, &info) == OST_SPE_SUCCESS);
    TEST_CHECK(ost_spe_write(file, destination) == OST_SPE_SUCCESS);
    TEST_CHECK(ost_spe_write(file, destination) == OST_SPE_ERR_IO);
    ost_spe_free(file);
    compare_files(source, destination);
}

static size_t make_spe2(unsigned char *bytes)
{
    size_t i;
    const unsigned char pixels[24] = {
        1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 0,
        7, 0, 8, 0, 9, 0, 10, 0, 11, 0, 12, 0,
    };

    for (i = 0; i < OST_SPE_HEADER_SIZE; ++i) {
        bytes[i] = (unsigned char)(i * 37u + 11u);
    }
    put16(bytes, 42u, 3u);
    put16(bytes, 108u, OST_SPE_UINT16);
    put16(bytes, 656u, 2u);
    put32(bytes, 1446u, 2u);
    put_float(bytes, 10u, 1.25f);
    memcpy(bytes + 20u, "01Jan2020", 10u);
    memcpy(bytes + 688u, "WinSpec/32 2.6", 15u);
    put_float(bytes, 1992u, 2.5f);
    memcpy(bytes + OST_SPE_HEADER_SIZE, pixels, sizeof(pixels));
    return OST_SPE_HEADER_SIZE + sizeof(pixels);
}

static void test_spe2(const char *directory)
{
    unsigned char bytes[OST_SPE_HEADER_SIZE + 24u];
    unsigned char frame[12] = {0};
    const unsigned char *header;
    size_t size;
    char source[512];
    char destination[512];
    ost_spe_file *file = NULL;
    ost_spe_info info;
    ost_spe_region region;

    size = make_spe2(bytes);
    TEST_CHECK(snprintf(source, sizeof(source), "%s/source.spe", directory) > 0);
    TEST_CHECK(snprintf(destination, sizeof(destination), "%s/roundtrip.spe", directory) > 0);
    write_file(source, bytes, size);
    TEST_CHECK(ost_spe_read(source, &file) == OST_SPE_SUCCESS);
    TEST_CHECK(ost_spe_get_info(file, &info) == OST_SPE_SUCCESS);
    TEST_CHECK(info.major_version == 2u);
    TEST_CHECK(info.header_version == 2.5f);
    TEST_CHECK(info.data_type == OST_SPE_UINT16);
    TEST_CHECK(info.width == 3u && info.height == 2u);
    TEST_CHECK(info.frame_count == 2u && info.region_count == 1u);
    TEST_CHECK(info.pixel_count == 6u);
    TEST_CHECK(info.exposure_seconds == 1.25);
    TEST_CHECK(strcmp(info.date, "01Jan2020") == 0);
    TEST_CHECK(strcmp(info.software_version, "WinSpec/32 2.6") == 0);
    TEST_CHECK(ost_spe_get_region(file, 0u, &region) == OST_SPE_SUCCESS);
    TEST_CHECK(region.width == 3u && region.height == 2u);
    TEST_CHECK(region.size == 12u && region.stride == 12u);
    TEST_CHECK(ost_spe_read_frame(file, 1u, frame, sizeof(frame), &size) == OST_SPE_SUCCESS);
    TEST_CHECK(size == sizeof(frame));
    TEST_CHECK(memcmp(frame, bytes + OST_SPE_HEADER_SIZE + 12u, sizeof(frame)) == 0);
    TEST_CHECK(ost_spe_read_frame(file, 2u, frame, sizeof(frame), &size) == OST_SPE_ERR_RANGE);
    TEST_CHECK(ost_spe_header(file, &header, &size) == OST_SPE_SUCCESS);
    TEST_CHECK(size == OST_SPE_HEADER_SIZE);
    TEST_CHECK(memcmp(header, bytes, OST_SPE_HEADER_SIZE) == 0);
    ost_spe_free(file);
    check_round_trip(source, destination);
}

static size_t make_spe3(unsigned char *bytes)
{
    static const char xml[] =
        "<SpeFormat version=\"3.0\" xmlns=\"http://www.princetoninstruments.com/spe/2009\">"
        "<DataFormat><DataBlock type=\"Frame\" count=\"2\" "
        "pixelFormat=\"MonochromeUnsigned16\" size=\"6\" stride=\"10\">"
        "<DataBlock type=\"Region\" count=\"1\" width=\"2\" height=\"1\" size=\"4\" stride=\"4\"/>"
        "<DataBlock type=\"Region\" count=\"1\" width=\"1\" height=\"1\" size=\"2\" stride=\"2\"/>"
        "</DataBlock></DataFormat></SpeFormat>";
    size_t frame;
    size_t i;

    memset(bytes, 0, OST_SPE_HEADER_SIZE + 20u + sizeof(xml));
    put_float(bytes, 1992u, 3.0f);
    put16(bytes, 108u, OST_SPE_UINT16);
    put64(bytes, 678u, OST_SPE_HEADER_SIZE + 20u);
    for (frame = 0; frame < 2u; ++frame) {
        for (i = 0; i < 6u; ++i) {
            bytes[OST_SPE_HEADER_SIZE + frame * 10u + i] = (unsigned char)(frame * 10u + i + 1u);
        }
        for (i = 6u; i < 10u; ++i) {
            bytes[OST_SPE_HEADER_SIZE + frame * 10u + i] = 0xa5u;
        }
    }
    memcpy(bytes + OST_SPE_HEADER_SIZE + 20u, xml, sizeof(xml));
    return OST_SPE_HEADER_SIZE + 20u + sizeof(xml) - 1u;
}

static void test_spe3(const char *directory)
{
    unsigned char bytes[OST_SPE_HEADER_SIZE + 1024u];
    unsigned char frame[6] = {0};
    const unsigned char *xml;
    size_t size;
    size_t footer_size;
    char source[512];
    char destination[512];
    ost_spe_file *file = NULL;
    ost_spe_info info;
    ost_spe_region region;

    size = make_spe3(bytes);
    TEST_CHECK(snprintf(source, sizeof(source), "%s/source3.spe", directory) > 0);
    TEST_CHECK(snprintf(destination, sizeof(destination), "%s/roundtrip3.spe", directory) > 0);
    write_file(source, bytes, size);
    TEST_CHECK(ost_spe_read(source, &file) == OST_SPE_SUCCESS);
    TEST_CHECK(ost_spe_get_info(file, &info) == OST_SPE_SUCCESS);
    TEST_CHECK(info.major_version == 3u);
    TEST_CHECK(info.data_type == OST_SPE_UINT16);
    TEST_CHECK(info.frame_count == 2u && info.region_count == 2u);
    TEST_CHECK(info.pixel_count == 3u);
    TEST_CHECK(ost_spe_get_region(file, 1u, &region) == OST_SPE_SUCCESS);
    TEST_CHECK(region.width == 1u && region.height == 1u);
    TEST_CHECK(region.size == 2u && region.stride == 2u);
    TEST_CHECK(ost_spe_read_frame(file, 1u, frame, sizeof(frame), &size) == OST_SPE_SUCCESS);
    TEST_CHECK(size == sizeof(frame));
    TEST_CHECK(memcmp(frame, bytes + OST_SPE_HEADER_SIZE + 10u, sizeof(frame)) == 0);
    TEST_CHECK(ost_spe_xml_footer(file, &xml, &footer_size) == OST_SPE_SUCCESS);
    TEST_CHECK(footer_size > 0u);
    TEST_CHECK(memcmp(xml, bytes + OST_SPE_HEADER_SIZE + 20u, footer_size) == 0);
    ost_spe_free(file);
    check_round_trip(source, destination);
}

static void test_create_uint16(const char *directory)
{
    const uint16_t frames[12] = {0u, 1u, 65535u, 42u, 1234u, 32768u,
                                 9u, 8u, 7u, 6u, 5u, 4u};
    const float temperature = -80.5f;
    uint16_t output[6] = {0};
    const unsigned char *xml;
    size_t xml_size;
    size_t size;
    size_t index;
    char path[512];
    ost_spe_file *file = NULL;
    ost_spe_info info;
    ost_spe_region region;

    TEST_CHECK(snprintf(path, sizeof(path), "%s/created-u16.spe", directory) > 0);
    TEST_CHECK(ost_spe_create(path, 3u, 2u, OST_SPE_UINT16, 2u, frames,
                              sizeof(frames), 0.125, &temperature) == OST_SPE_SUCCESS);
    TEST_CHECK(ost_spe_create(path, 3u, 2u, OST_SPE_UINT16, 2u, frames,
                              sizeof(frames), 0.125, &temperature) == OST_SPE_ERR_IO);
    TEST_CHECK(ost_spe_read(path, &file) == OST_SPE_SUCCESS);
    TEST_CHECK(ost_spe_get_info(file, &info) == OST_SPE_SUCCESS);
    TEST_CHECK(info.major_version == 3u && info.header_version == 3.0f);
    TEST_CHECK(info.data_type == OST_SPE_UINT16 && info.pixel_size == 2u);
    TEST_CHECK(info.width == 3u && info.height == 2u);
    TEST_CHECK(info.frame_count == 2u && info.region_count == 1u);
    TEST_CHECK(info.pixel_count == 6u && info.exposure_seconds == 0.125);
    TEST_CHECK(info.temperature_setpoint == temperature);
    TEST_CHECK(ost_spe_get_region(file, 0u, &region) == OST_SPE_SUCCESS);
    TEST_CHECK(region.width == 3u && region.height == 2u);
    TEST_CHECK(region.size == sizeof(output) && region.stride == sizeof(output));
    for (index = 0u; index < 2u; ++index) {
        memset(output, 0, sizeof(output));
        TEST_CHECK(ost_spe_read_frame(file, index, output, sizeof(output), &size) == OST_SPE_SUCCESS);
        TEST_CHECK(size == sizeof(output));
        TEST_CHECK(memcmp(output, frames + index * 6u, sizeof(output)) == 0);
    }
    TEST_CHECK(ost_spe_xml_footer(file, &xml, &xml_size) == OST_SPE_SUCCESS);
    TEST_CHECK(xml_size > 0u);
    TEST_CHECK(strstr((const char *)xml, "MonochromeUnsigned16") != NULL);
    TEST_CHECK(strstr((const char *)xml, "<Experiment") == NULL);
    ost_spe_free(file);
}

static void test_create_float32(const char *directory)
{
    const float frames[8] = {0.0f, -1.5f, 2.25f, 1000.0f,
                             3.5f, 4.75f, -0.125f, 65535.0f};
    float output[4] = {0};
    const unsigned char *xml;
    size_t xml_size;
    size_t size;
    size_t index;
    char path[512];
    ost_spe_file *file = NULL;
    ost_spe_info info;

    TEST_CHECK(snprintf(path, sizeof(path), "%s/created-f32.spe", directory) > 0);
    TEST_CHECK(ost_spe_create(path, 2u, 2u, OST_SPE_FLOAT32, 2u, frames,
                              sizeof(frames), 0.5, NULL) == OST_SPE_SUCCESS);
    TEST_CHECK(ost_spe_read(path, &file) == OST_SPE_SUCCESS);
    TEST_CHECK(ost_spe_get_info(file, &info) == OST_SPE_SUCCESS);
    TEST_CHECK(info.major_version == 3u && info.data_type == OST_SPE_FLOAT32);
    TEST_CHECK(info.width == 2u && info.height == 2u && info.pixel_count == 4u);
    TEST_CHECK(info.frame_count == 2u && info.pixel_size == 4u);
    TEST_CHECK(info.exposure_seconds == 0.5 && info.temperature_setpoint == 0.0f);
    for (index = 0u; index < 2u; ++index) {
        memset(output, 0, sizeof(output));
        TEST_CHECK(ost_spe_read_frame(file, index, output, sizeof(output), &size) == OST_SPE_SUCCESS);
        TEST_CHECK(size == sizeof(output));
        TEST_CHECK(memcmp(output, frames + index * 4u, sizeof(output)) == 0);
    }
    TEST_CHECK(ost_spe_xml_footer(file, &xml, &xml_size) == OST_SPE_SUCCESS);
    TEST_CHECK(xml_size > 0u);
    TEST_CHECK(strstr((const char *)xml, "MonochromeFloating32") != NULL);
    TEST_CHECK(strstr((const char *)xml, "<Experiment") == NULL);
    ost_spe_free(file);
}

static void test_provenance(const char *directory)
{
    uint16_t frame[200] = {0};
    ost_spe_metadata metadata;
    ost_spe_metadata legacy;
    ost_spe_file *file = NULL;
    ost_spe_info info;
    const unsigned char *xml;
    size_t xml_size;
    char path[512];

    TEST_CHECK(snprintf(path, sizeof(path), "%s/provenance.spe", directory) > 0);
    ost_spe_metadata_init(&metadata);
    metadata.exposure_seconds = 59.904;
    metadata.requested_exposure_known = 1;
    metadata.requested_exposure_seconds = 60.0;
    metadata.accumulations = 3u;
    metadata.readout_rate_hz = 100000u;
    metadata.roi_known = 1;
    metadata.roi_x = 10u;
    metadata.roi_y = 20u;
    metadata.roi_width = 40u;
    metadata.roi_height = 40u;
    metadata.x_binning = 2u;
    metadata.y_binning = 4u;
    metadata.requested_roi_known = 1;
    metadata.requested_roi_x = 10u;
    metadata.requested_roi_y = 20u;
    metadata.requested_roi_width = 41u;
    metadata.requested_roi_height = 41u;
    metadata.requested_x_binning = 2u;
    metadata.requested_y_binning = 4u;
    (void)snprintf(metadata.target_source, sizeof(metadata.target_source), "option");
    metadata.lock_tolerance_known = 1;
    metadata.lock_tolerance_celsius = 0.0;
    metadata.lock_hold_known = 1;
    metadata.lock_hold_seconds = 0.0;
    metadata.lock_settle_known = 1;
    metadata.lock_settle_seconds = 12.5;
    metadata.stream_known = 1;
    metadata.stream_frames_delivered = 2u;
    metadata.stream_short_transfers = 1u;
    metadata.stream_max_ready = 3u;
    metadata.stream_max_callback_us = 1200u;
    metadata.stream_bytes_received = UINT64_C(4294967297);
    metadata.stream_transfers_completed = 17u;
    metadata.stream_partial_bytes_discarded = 512u;
    (void)snprintf(metadata.calibration_status, sizeof(metadata.calibration_status), "calibrated");
    TEST_CHECK(ost_spe_create_with_metadata(path, 20u, 10u, OST_SPE_UINT16, 1u, frame, sizeof(frame),
                                            &metadata) == OST_SPE_SUCCESS);
    TEST_CHECK(ost_spe_read(path, &file) == OST_SPE_SUCCESS);
    TEST_CHECK(ost_spe_get_info(file, &info) == OST_SPE_SUCCESS);
    TEST_CHECK(info.exposure_seconds > 59.903 && info.exposure_seconds < 59.905);
    TEST_CHECK(ost_spe_get_metadata(file, &legacy) == OST_SPE_SUCCESS);
    TEST_CHECK(legacy.accumulations == 3u && legacy.readout_rate_hz == 100000u);
    TEST_CHECK(legacy.roi_width == 40u && legacy.x_binning == 2u);
    TEST_CHECK(legacy.requested_exposure_known == 0 && legacy.requested_roi_known == 0);
    TEST_CHECK(ost_spe_xml_footer(file, &xml, &xml_size) == OST_SPE_SUCCESS);
    TEST_CHECK(strstr((const char *)xml,
        "<Provenance xmlns=\"urn:openst133:provenance:1\"><Exposure requestedSeconds=\"60\" "
        "effectiveSeconds=\"") != NULL);
    TEST_CHECK(strstr((const char *)xml, "generatedValueRule=\"floor\"/>") != NULL);
    TEST_CHECK(strstr((const char *)xml,
        "<RequestedRegion x=\"10\" y=\"20\" width=\"41\" height=\"41\" xBinning=\"2\" yBinning=\"4\"/>") != NULL);
    TEST_CHECK(strstr((const char *)xml,
        "<EffectiveRegion x=\"10\" y=\"20\" width=\"40\" height=\"40\" xBinning=\"2\" yBinning=\"4\"/>") != NULL);
    TEST_CHECK(strstr((const char *)xml, "<Selection targetSource=\"option\"/>") != NULL);
    TEST_CHECK(strstr((const char *)xml,
        "<SoftwareLock toleranceCelsius=\"0\" holdSeconds=\"0\" settleSeconds=\"12.5\"/>") != NULL);
    TEST_CHECK(strstr((const char *)xml, "bytesReceived=\"4294967297\"") != NULL);
    TEST_CHECK(strstr((const char *)xml, "stopped=\"false\"") != NULL);
    TEST_CHECK(strstr((const char *)xml, "<Calibration status=\"calibrated\"/>") != NULL);
    ost_spe_free(file);
}

static void test_provenance_rules(const char *directory)
{
    static const struct {
        int requested_known;
        const char *rule;
    } cases[] = {
        {0, "unknown"},
        {1, "floor"},
    };
    uint16_t frame = 0;
    size_t index;

    for (index = 0u; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        ost_spe_metadata metadata;
        ost_spe_file *file = NULL;
        const unsigned char *xml;
        size_t xml_size;
        char path[512];
        char expected[96];

        TEST_CHECK(snprintf(path, sizeof(path), "%s/rules.spe", directory) > 0);
        ost_spe_metadata_init(&metadata);
        metadata.exposure_seconds = 0.125;
        metadata.requested_exposure_known = cases[index].requested_known;
        metadata.requested_exposure_seconds = 0.125;
        TEST_CHECK(ost_spe_create_with_metadata(path, 1u, 1u, OST_SPE_UINT16, 1u, &frame, sizeof(frame),
                                                &metadata) == OST_SPE_SUCCESS);
        TEST_CHECK(ost_spe_read(path, &file) == OST_SPE_SUCCESS);
        TEST_CHECK(ost_spe_xml_footer(file, &xml, &xml_size) == OST_SPE_SUCCESS);
        TEST_CHECK(strstr((const char *)xml,
            "<Provenance xmlns=\"urn:openst133:provenance:1\"><Exposure") != NULL);
        TEST_CHECK(snprintf(expected, sizeof(expected), "generatedValueRule=\"%s\"/>", cases[index].rule) > 0);
        TEST_CHECK(strstr((const char *)xml, expected) != NULL);
        TEST_CHECK(strstr((const char *)xml, "<Calibration status=\"uncalibrated\"/>") != NULL);
        ost_spe_free(file);
        TEST_CHECK(remove(path) == 0);
    }
}

static void test_lab_file(const char *path)
{
    ost_spe_file *file = NULL;
    ost_spe_info info;

    TEST_CHECK(ost_spe_read(path, &file) == OST_SPE_SUCCESS);
    TEST_CHECK(ost_spe_get_info(file, &info) == OST_SPE_SUCCESS);
    printf("major_version=%u header_version=%.9g width=%u height=%u data_type=%d frames=%llu exposure_seconds=%.9g date=%s\n",
           info.major_version, (double)info.header_version, info.width, info.height, info.data_type,
           (unsigned long long)info.frame_count, info.exposure_seconds, info.date);
    ost_spe_free(file);
}

int main(int argc, char **argv)
{
    char directory[] = "/tmp/ost-spe-test-XXXXXX";
    TEST_CHECK(test_mkdtemp(directory) != NULL);
    test_spe2(directory);
    test_spe3(directory);
    test_create_uint16(directory);
    test_create_float32(directory);
    test_provenance(directory);
    test_provenance_rules(directory);
    if (argc == 2) {
        test_lab_file(argv[1]);
    } else {
        TEST_CHECK(argc == 1);
    }
    return 0;
}
