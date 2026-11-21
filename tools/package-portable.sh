#!/usr/bin/env bash
# Builds the portable package (docs/design/launcher.md, "One portable folder"): the `portable`
# preset (SDL3, FFmpeg and libstdc++ linked statically, cmake/portable.cmake), installed into
# Bloodborne-Linux/ and packed as Bloodborne-Linux-<version>-x86_64.tar.gz (docs/releasing.md).
#
# Usage: tools/package-portable.sh [OUTPUT_DIR]   (default: build/portable/package)
# Needs what the release preset needs, plus the static libstdc++, the development headers SDL
# builds its Wayland (with libdecor), X11 and audio (PipeWire, PulseAudio, ALSA) backends from, and
# nasm for FFmpeg's x86 assembly (without it movies decode slower). Fedora: libstdc++-static
# libdecor-devel pipewire-devel pulseaudio-libs-devel alsa-lib-devel nasm. CMake names what is
# missing. Releases build it in the sniper SDK (tools/release/build-package.sh, docs/releasing.md).
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
out=$(realpath -m "${1:-$root/build/portable/package}")  # relative to the caller's directory
cd "$root"

cmake --preset portable
cmake --build --preset portable --target bb-run bb-launcher

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
folder=$stage/Bloodborne-Linux
cmake --install build/portable --prefix "$folder" --strip

# Nothing of the build machine that a player may not have: SDL3, FFmpeg and libstdc++ are inside.
status=0
for bin in bb-launcher bb-run; do
    if ! deps=$(ldd "$folder/$bin"); then
        echo "package-portable: ldd failed on $bin" >&2
        exit 1
    fi
    bad=$(grep -E 'not found|libSDL3|libav(codec|format|util)|libsw(scale|resample)|libstdc\+\+|libgcc_s' <<<"$deps" || true)
    if [[ -n $bad ]]; then
        echo "package-portable: $bin links libraries that belong in the package:" >&2
        echo "$bad" >&2
        status=1
    fi
done
[[ $status -eq 0 ]] || exit 1

# The newest glibc symbol version the programs need decides the oldest system they start on
# (docs/releasing.md, "Build base"). BB_GLIBC_MAX makes a newer one an error, as the release build
# sets it.
glibc=$(objdump -T "$folder/bb-run" "$folder/bb-launcher" | grep -oE 'GLIBC_[0-9]+(\.[0-9]+)+' |
    sed 's/^GLIBC_//' | sort -Vu | tail -1)
echo "package-portable: needs glibc $glibc or newer" >&2
newest=$(printf '%s\n' "$glibc" "${BB_GLIBC_MAX:-$glibc}" | sort -V | tail -1)
if [[ $newest != "${BB_GLIBC_MAX:-$glibc}" ]]; then
    echo "package-portable: glibc $glibc is newer than BB_GLIBC_MAX=$BB_GLIBC_MAX" >&2
    exit 1
fi

# The version the programs were built with (cmake/version.cmake): v0.2.0 on a release tag,
# v0.2.0-3-gabc1234 after it, with -dirty for uncommitted changes.
version=$("$folder/bb-run" --version)
version=${version#bb-run }
mkdir -p "$out"
archive=$out/Bloodborne-Linux-$version-x86_64.tar.gz
# Times of the commit and a fixed order: the same commit packs the same archive.
tar -C "$stage" --owner=0 --group=0 --numeric-owner --sort=name \
    --mtime="@$(git log -1 --format=%ct)" -cf - Bloodborne-Linux | gzip -n > "$archive"
echo "$archive"
