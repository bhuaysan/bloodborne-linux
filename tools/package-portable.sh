#!/usr/bin/env bash
# Builds the portable package (docs/design/launcher.md, "One portable folder"): the `portable`
# preset (SDL3, FFmpeg and libstdc++ linked statically, cmake/portable.cmake), installed into
# Bloodborne-Linux/ and packed as Bloodborne-Linux-<commit>.tar.gz.
#
# Usage: tools/package-portable.sh [OUTPUT_DIR]   (default: build/portable/package)
# Needs what the release preset needs, plus the static libstdc++, the development headers SDL
# builds its Wayland (with libdecor), X11 and audio (PipeWire, PulseAudio, ALSA) backends from, and
# nasm for FFmpeg's x86 assembly (without it movies decode slower). Fedora: libstdc++-static
# libdecor-devel pipewire-devel pulseaudio-libs-devel alsa-lib-devel nasm. CMake names what is
# missing.
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

commit=$(git rev-parse --short HEAD)
if [[ -n $(git status --porcelain --untracked-files=no) ]]; then
    commit+=-dirty
fi
mkdir -p "$out"
archive=$out/Bloodborne-Linux-$commit.tar.gz
# Times of the commit and a fixed order: the same commit packs the same archive.
tar -C "$stage" --owner=0 --group=0 --numeric-owner --sort=name \
    --mtime="@$(git log -1 --format=%ct)" -cf - Bloodborne-Linux | gzip -n > "$archive"
echo "$archive"
