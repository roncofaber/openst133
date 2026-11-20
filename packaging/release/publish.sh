#!/usr/bin/env bash
# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT
# Check the sdist and wheels, write SHA256SUMS and create the GitHub Release for a pushed tag.

set -euo pipefail

if [[ $# -ne 2 ]]; then
    printf 'usage: %s <tag> <asset-directory>\n' "$0" >&2
    exit 64
fi

tag=$1
dist=$2
root=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
version=${tag#v}

for pattern in \
    "openst133-$version.tar.gz" \
    "openst133-$version-*manylinux*x86_64*.whl" \
    "openst133-$version-*win_amd64.whl"; do
    if ! compgen -G "$dist/$pattern" > /dev/null; then
        printf 'missing release asset: %s\n' "$pattern" >&2
        exit 1
    fi
done
if [[ -e "$dist/SHA256SUMS" ]]; then
    printf 'refusing existing %s\n' "$dist/SHA256SUMS" >&2
    exit 65
fi

notes=$(mktemp)
trap 'rm -f -- "$notes"' EXIT
awk -v header="## [$version]" 'index($0, header) == 1 { found = 1; next } /^## \[/ { found = 0 } found' "$root/CHANGELOG.md" > "$notes"
if ! grep -q '[^[:space:]]' "$notes"; then
    printf 'CHANGELOG.md has no notes for %s\n' "$version" >&2
    exit 1
fi

sums=$(cd "$dist" && sha256sum -- *)
printf '%s\n' "$sums" > "$dist/SHA256SUMS"
(cd "$dist" && sha256sum --check --quiet --strict SHA256SUMS)
gh release create "$tag" "$dist"/* --verify-tag --title "openst133 $version" --notes-file "$notes"
