# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)
project(openst133 VERSION 0.1.0 LANGUAGES C)

include(CTest)
include(GNUInstallDirs)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBUSB QUIET IMPORTED_TARGET libusb-1.0)
endif()
if(TARGET PkgConfig::LIBUSB)
    set(OPENST133_LIBUSB_TARGET PkgConfig::LIBUSB)
else()
    find_path(OPENST133_LIBUSB_INCLUDE_DIR NAMES libusb.h PATH_SUFFIXES libusb-1.0)
    find_library(OPENST133_LIBUSB_LIBRARY NAMES libusb-1.0 usb-1.0)
    if(NOT OPENST133_LIBUSB_INCLUDE_DIR OR NOT OPENST133_LIBUSB_LIBRARY)
        message(FATAL_ERROR "libusb 1.0 was not found through pkg-config or CMake search paths")
    endif()
    add_library(openst133_libusb UNKNOWN IMPORTED)
    set_target_properties(openst133_libusb PROPERTIES
        IMPORTED_LOCATION "${OPENST133_LIBUSB_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${OPENST133_LIBUSB_INCLUDE_DIR}"
    )
    set(OPENST133_LIBUSB_TARGET openst133_libusb)
endif()

add_library(openst133 SHARED
    src/camera.c
    src/camera_acquire.c
    src/camera_modes.c
    src/camera_recover.c
    src/camera_scan.c
    src/camera_settings.c
    src/camera_stream.c
    src/camera_startup.c
    src/hw_guard.c
    src/openst133.c
    src/libusb_transport.c
    src/pcap_usbmon.c
    src/protocol.c
    src/replay_transport.c
    src/spe.c
    src/time_util.c
)
add_library(openst133::openst133 ALIAS openst133)
target_compile_features(openst133 PUBLIC c_std_11)
set_target_properties(openst133 PROPERTIES
    C_EXTENSIONS OFF
    C_STANDARD_REQUIRED ON
)
target_compile_options(openst133 PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(openst133 PRIVATE OPENST133_VERSION="${PROJECT_VERSION}")
target_include_directories(openst133
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        src
)
find_package(Threads REQUIRED)
target_link_libraries(openst133 PRIVATE ${OPENST133_LIBUSB_TARGET} Threads::Threads)
find_library(OPENST133_MATH_LIBRARY m)
if(OPENST133_MATH_LIBRARY)
    target_link_libraries(openst133 PRIVATE ${OPENST133_MATH_LIBRARY})
endif()
if(WIN32)
    set_target_properties(openst133 PROPERTIES PREFIX "")
endif()

add_executable(ost src/cli/ost.c tools/ost-temperature.c)
target_compile_features(ost PRIVATE c_std_11)
target_compile_options(ost PRIVATE -Wall -Wextra -Werror)
target_link_libraries(ost PRIVATE openst133)
if(OPENST133_MATH_LIBRARY)
    target_link_libraries(ost PRIVATE ${OPENST133_MATH_LIBRARY})
endif()
if(UNIX AND NOT APPLE)
    if(SKBUILD)
        set(OPENST133_OST_RPATH "$ORIGIN")
    else()
        set(OPENST133_OST_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
    endif()
    set_target_properties(ost PROPERTIES
        INSTALL_RPATH "${OPENST133_OST_RPATH}"
    )
endif()

configure_file(packaging/openst133.pc.in openst133.pc @ONLY)

install(TARGETS openst133 ost
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Runtime
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Runtime
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Development
)
install(DIRECTORY include/
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    COMPONENT Development
    FILES_MATCHING PATTERN "*.h"
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/openst133.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
    COMPONENT Development
)
install(FILES packaging/udev/60-openst133.rules
    DESTINATION "${CMAKE_INSTALL_DATADIR}/openst133/udev"
    COMPONENT Runtime
)
install(TARGETS openst133 ost
    EXCLUDE_FROM_ALL
    COMPONENT python
    LIBRARY DESTINATION .
    RUNTIME DESTINATION .
)
install(FILES packaging/udev/60-openst133.rules
    EXCLUDE_FROM_ALL
    COMPONENT python
    DESTINATION .
)

if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    add_executable(test_openst133 tests/test_openst133.c)
    target_compile_options(test_openst133 PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(test_openst133 PRIVATE openst133)
    add_test(NAME c_api COMMAND test_openst133)

    add_executable(test_replay tests/test_replay.c)
    target_compile_options(test_replay PRIVATE -Wall -Wextra -Werror)
    target_include_directories(test_replay PRIVATE src)
    target_link_libraries(test_replay PRIVATE openst133)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests/data")
    add_test(NAME replay COMMAND test_replay "${CMAKE_CURRENT_BINARY_DIR}/tests/data")

    add_executable(test_protocol tests/test_protocol.c)
    target_compile_options(test_protocol PRIVATE -Wall -Wextra -Werror)
    target_include_directories(test_protocol PRIVATE src)
    target_link_libraries(test_protocol PRIVATE openst133)
    add_test(NAME protocol COMMAND test_protocol "${CMAKE_CURRENT_BINARY_DIR}/tests/data")

    add_executable(test_robustness tests/test_robustness.c)
    target_compile_features(test_robustness PRIVATE c_std_11)
    target_compile_options(test_robustness PRIVATE -Wall -Wextra -Werror)
    target_include_directories(test_robustness PRIVATE src)
    target_link_libraries(test_robustness PRIVATE openst133)
    add_test(NAME robustness COMMAND test_robustness)

    add_executable(test_identity_guard tests/test_identity_guard.c)
    target_compile_features(test_identity_guard PRIVATE c_std_11)
    target_compile_options(test_identity_guard PRIVATE -Wall -Wextra -Werror)
    target_include_directories(test_identity_guard PRIVATE src)
    target_link_libraries(test_identity_guard PRIVATE openst133)
    add_test(NAME identity_guard COMMAND test_identity_guard)

    add_executable(test_spe tests/test_spe.c)
    target_compile_options(test_spe PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(test_spe PRIVATE openst133)
    add_test(NAME spe_io COMMAND test_spe)

    add_executable(test_camera tests/test_camera.c)
    target_compile_options(test_camera PRIVATE -Wall -Wextra -Werror)
    target_include_directories(test_camera PRIVATE src)
    target_link_libraries(test_camera PRIVATE openst133)
    add_test(NAME camera_api COMMAND test_camera "${CMAKE_CURRENT_BINARY_DIR}/tests/data")

    add_executable(test_camera_recover tests/test_camera_recover.c)
    target_compile_features(test_camera_recover PRIVATE c_std_11)
    target_compile_options(test_camera_recover PRIVATE -Wall -Wextra -Werror)
    target_include_directories(test_camera_recover PRIVATE src)
    target_link_libraries(test_camera_recover PRIVATE openst133)
    add_test(NAME camera_recover COMMAND test_camera_recover)

    add_executable(test_hw_guard tests/test_hw_guard.c)
    target_compile_features(test_hw_guard PRIVATE c_std_11)
    target_compile_options(test_hw_guard PRIVATE -Wall -Wextra -Werror)
    target_include_directories(test_hw_guard PRIVATE src)
    target_link_libraries(test_hw_guard PRIVATE openst133)
    add_test(NAME hw_guard COMMAND test_hw_guard "${CMAKE_CURRENT_BINARY_DIR}/tests/data")

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(test_hw_busy tests/test_hw_busy.c)
        target_compile_features(test_hw_busy PRIVATE c_std_11)
        target_compile_options(test_hw_busy PRIVATE -Wall -Wextra -Werror)
        target_include_directories(test_hw_busy PRIVATE src)
        target_link_libraries(test_hw_busy PRIVATE openst133 ${OPENST133_LIBUSB_TARGET})
        add_test(NAME hw_busy COMMAND test_hw_busy "${CMAKE_CURRENT_BINARY_DIR}/tests/data")

        add_executable(test_stream_cleanup tests/test_stream_cleanup.c)
        target_compile_features(test_stream_cleanup PRIVATE c_std_11)
        target_compile_options(test_stream_cleanup PRIVATE -Wall -Wextra -Werror)
        target_include_directories(test_stream_cleanup PRIVATE src)
        target_link_libraries(test_stream_cleanup PRIVATE openst133 ${OPENST133_LIBUSB_TARGET})
        add_test(NAME stream_cleanup COMMAND test_stream_cleanup)
    endif()

    add_executable(test_camera_threads tests/test_camera_threads.c)
    target_compile_features(test_camera_threads PRIVATE c_std_11)
    target_compile_options(test_camera_threads PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(test_camera_threads PRIVATE openst133 Threads::Threads)
    add_test(NAME camera_threads COMMAND test_camera_threads)

    add_executable(test_cli tests/test_cli.c)
    target_compile_options(test_cli PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(test_cli PRIVATE openst133)
    add_test(NAME cli COMMAND test_cli $<TARGET_FILE:ost>)

    add_executable(test_review_regressions tests/test_review_regressions.c)
    target_compile_features(test_review_regressions PRIVATE c_std_11)
    target_compile_options(test_review_regressions PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(test_review_regressions PRIVATE openst133)
    if(OPENST133_MATH_LIBRARY)
        target_link_libraries(test_review_regressions PRIVATE ${OPENST133_MATH_LIBRARY})
    endif()
    add_test(NAME review_regressions COMMAND test_review_regressions "${CMAKE_CURRENT_BINARY_DIR}/tests/data")

    add_executable(test_review_followups tests/test_review_followups.c)
    target_compile_features(test_review_followups PRIVATE c_std_11)
    target_compile_options(test_review_followups PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(test_review_followups PRIVATE openst133)
    if(OPENST133_MATH_LIBRARY)
        target_link_libraries(test_review_followups PRIVATE ${OPENST133_MATH_LIBRARY})
    endif()
    add_test(NAME review_followups COMMAND test_review_followups)

    add_executable(test_cli_geometry tests/test_cli_geometry.c)
    target_compile_features(test_cli_geometry PRIVATE c_std_11)
    target_compile_options(test_cli_geometry PRIVATE -Wall -Wextra -Werror)
    target_include_directories(test_cli_geometry PRIVATE src)
    target_link_libraries(test_cli_geometry PRIVATE openst133)
    if(OPENST133_MATH_LIBRARY)
        target_link_libraries(test_cli_geometry PRIVATE ${OPENST133_MATH_LIBRARY})
    endif()
    add_test(NAME cli_geometry COMMAND test_cli_geometry)

    add_test(
        NAME python_binding
        COMMAND "${Python3_EXECUTABLE}" -I -m unittest
    )
    set_tests_properties(python_binding PROPERTIES
        ENVIRONMENT "OPENST133_LIBRARY=$<TARGET_FILE:openst133>"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/python"
    )
endif()
