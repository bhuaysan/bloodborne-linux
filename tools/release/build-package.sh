#!/usr/bin/env bash
# Builds the portable package of the committed HEAD in the release build image, as the release
# workflow does (docs/releasing.md, "Build base"): the Steam Runtime 3 "sniper" SDK, so the package
# starts on glibc 2.31 and newer.
#
# Usage: tools/release/build-package.sh [OUTPUT_DIR]   (default: build/release-package)
#
# Writes the tar.gz and the AppImage. Needs podman and a network (the image, nasm, squashfs-tools,
# CMake, the AppImage runtime and the pinned sources of cmake/*.cmake). The
# build runs on a clone of HEAD in a new directory under build/ (not $TMPDIR, which may be RAM
# and count against the container's memory limit), removed afterwards. Uncommitted changes are not
# built.
set -euo pipefail

# The image the release workflow uses (.github/workflows/release.yml), pinned by digest.
image=registry.gitlab.steamos.cloud/steamrt/sniper/sdk@sha256:bb30053d3cd19d35c76ae69b2790dbaa1430448006f8240cb6d7e723f2d1940e

root=$(cd "$(dirname "$0")/../.." && pwd)
out=$(realpath -m "${1:-$root/build/release-package}")
command -v podman >/dev/null || { echo "build-package: podman not found" >&2; exit 1; }

mkdir -p "$root/build"
work=$(mktemp -d "$root/build/package-work.XXXXXX")
trap 'rm -rf "$work"' EXIT
# A clone with the tags, so the version (cmake/version.cmake) is HEAD's.
git clone -q --no-hardlinks "$root" "$work/src"
git -C "$work/src" checkout -q --detach "$(git -C "$root" rev-parse HEAD)"

# Memory-limited, no swap: the machine has other work.
podman run --rm --memory 12g --memory-swap 12g \
    -e CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-6}" \
    -v "$work:/work:rw,Z" "$image" bash -c '
        set -euo pipefail
        git config --global --add safe.directory /work/src
        cd /work/src
        env=$(tools/release/sniper-setup.sh)
        eval "$env"
        tools/package-portable.sh /work/out'
mkdir -p "$out"
cp "$work"/out/*.tar.gz "$work"/out/*.AppImage "$out"/
ls "$out"
