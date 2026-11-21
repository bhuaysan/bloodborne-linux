#!/usr/bin/env bash
# Prepares the release build image, the Steam Runtime 3 "sniper" SDK, for tools/package-portable.sh
# (docs/releasing.md, "Build base"). Runs inside that image as root: the release workflow's job
# container, or tools/release/build-package.sh locally.
#
# Usage: env=$(tools/release/sniper-setup.sh) && eval "$env"
#        (not eval "$(...)", which would hide a failure of this script)
#
# The image has GCC 14 (as g++-14) and the headers of every SDL backend, on glibc 2.31. It lacks
# nasm (FFmpeg's x86 assembly; from the image's Debian archive) and a CMake new enough for
# CMakeLists.txt (3.25 there; the pinned Kitware release below goes to /opt/cmake). Prints the
# environment the build needs, for eval.
set -euo pipefail

cmake_version=4.3.6
cmake_sha256=fba967b44c7b40f155f9492dacb5477aab8333699986e1e07a7d90803aba7219

if ! command -v nasm >/dev/null; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq >&2
    apt-get install -y -qq --no-install-recommends nasm >&2
fi

if [[ ! -x /opt/cmake/bin/cmake ]]; then
    archive=$(mktemp)
    curl -fsSL -o "$archive" \
        "https://github.com/Kitware/CMake/releases/download/v$cmake_version/cmake-$cmake_version-linux-x86_64.tar.gz"
    echo "$cmake_sha256  $archive" | sha256sum -c --quiet >&2
    mkdir -p /opt/cmake
    tar -xzf "$archive" -C /opt/cmake --strip-components=1
    rm -f "$archive"
fi

echo "export PATH=/opt/cmake/bin:\$PATH CC=gcc-14 CXX=g++-14 BB_GLIBC_MAX=2.31"
