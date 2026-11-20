#!/usr/bin/env bash
# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT
# Cross-build openst133.dll and ost.exe, or skip when MinGW prerequisites are absent.

set -euo pipefail

ROOT="$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)"
TOOLCHAIN="$ROOT/cmake/mingw-w64-x86_64.cmake"
# One build directory per checkout, so worktrees never share a CMake cache.
BUILD_DIR="${OPENST133_WINDOWS_BUILD_DIR:-${TMPDIR:-/tmp}/openst133/windows-x86_64-$(printf '%s' "$ROOT" | sha256sum | cut -c1-12)}"
TRIPLE=x86_64-w64-mingw32
missing=()

for tool in cmake ninja pkg-config "$TRIPLE-gcc"; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        missing+=("$tool")
    fi
done

pc_dir_for_root() {
    local root="$1"
    local subdir

    for subdir in lib/pkgconfig lib64/pkgconfig share/pkgconfig; do
        if [[ -f "$root/$subdir/libusb-1.0.pc" && -f "$root/include/libusb-1.0/libusb.h" ]]; then
            printf '%s\n' "$root/$subdir"
            return 0
        fi
    done
    return 1
}

LIBUSB_ROOT="${OPENST133_LIBUSB_ROOT:-}"
LIBUSB_PC_DIR=""
if [[ -n "$LIBUSB_ROOT" ]]; then
    LIBUSB_PC_DIR="$(pc_dir_for_root "$LIBUSB_ROOT" || true)"
else
    for candidate in "$ROOT/.deps/libusb-$TRIPLE" "/usr/$TRIPLE"; do
        if LIBUSB_PC_DIR="$(pc_dir_for_root "$candidate")"; then
            LIBUSB_ROOT="$candidate"
            break
        fi
    done
fi
if [[ -z "$LIBUSB_PC_DIR" ]]; then
    missing+=("libusb-1.0 for $TRIPLE")
fi

if ((${#missing[@]} != 0)); then
    printf 'SKIP: Windows cross-build prerequisites missing: %s\n' "${missing[*]}"
    printf '%s\n' 'Compiler and build tools (not installed by this script):'
    printf '%s\n' '  sudo apt update'
    printf '%s\n' '  sudo apt install gcc-mingw-w64-x86-64 binutils-mingw-w64-x86-64 cmake ninja-build pkg-config'
    printf '%s\n' 'MinGW libusb must also be installed or cross-built, for example with .github/scripts/build-mingw-libusb.sh PREFIX WORKDIR.'
    printf '%s\n' 'Set OPENST133_LIBUSB_ROOT to its prefix when it is not in the default search path.'
    exit 0
fi

export OPENST133_LIBUSB_ROOT="$LIBUSB_ROOT"
export PKG_CONFIG_LIBDIR="$LIBUSB_PC_DIR"
export PKG_CONFIG_PATH=""
export PKG_CONFIG_SYSROOT_DIR=""

if ! pkg-config --exists libusb-1.0; then
    printf 'ERROR: %s does not describe a usable libusb-1.0 for %s\n' "$LIBUSB_PC_DIR" "$TRIPLE" >&2
    exit 1
fi

cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF
cmake --build "$BUILD_DIR" --target openst133 ost --parallel

if [[ ! -f "$BUILD_DIR/openst133.dll" || ! -f "$BUILD_DIR/ost.exe" ]]; then
    printf 'ERROR: build completed without openst133.dll and ost.exe in %s\n' "$BUILD_DIR" >&2
    exit 1
fi

printf 'PASS: built %s and %s\n' "$BUILD_DIR/openst133.dll" "$BUILD_DIR/ost.exe"
