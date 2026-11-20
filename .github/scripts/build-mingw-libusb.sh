#!/usr/bin/env bash
# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT
# Download libusb 1.0.30, check its SHA-256 and cross-build it for x86_64-w64-mingw32 into a prefix.

set -euo pipefail

if [[ $# -ne 2 ]]; then
    printf 'usage: %s <install-prefix> <new-work-directory>\n' "$0" >&2
    exit 64
fi

version=1.0.30
sha256=fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf
prefix=$1
work=$2

if [[ -e "$work" ]]; then
    printf 'refusing existing work directory: %s\n' "$work" >&2
    exit 65
fi
mkdir -p -- "$work/build"
archive="$work/libusb-$version.tar.bz2"
curl --fail --silent --show-error --location --proto '=https' --tlsv1.2 --output "$archive" \
    "https://github.com/libusb/libusb/releases/download/v$version/libusb-$version.tar.bz2"
printf '%s  %s\n' "$sha256" "$archive" | sha256sum --check --strict
tar -xjf "$archive" -C "$work" --no-same-owner
cd "$work/build"
"$work/libusb-$version/configure" --quiet --host=x86_64-w64-mingw32 --prefix="$prefix" --enable-shared --disable-static
make --silent -j"$(nproc)"
make --silent install
test -f "$prefix/include/libusb-1.0/libusb.h"
printf 'libusb %s for x86_64-w64-mingw32 installed in %s\n' "$version" "$prefix"
