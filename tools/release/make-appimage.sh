#!/usr/bin/env bash
# Writes the AppImage of an install tree (docs/design/launcher.md, "One portable folder"): the
# pinned type2 runtime, then a squashfs of the tree. Called by tools/package-portable.sh; runs
# alone as well, which is how the assembly is tested without a full build.
#
# Usage: tools/release/make-appimage.sh INSTALL_TREE OUTPUT_FILE [CACHE_DIR]
#   INSTALL_TREE  the folder `cmake --install` wrote (bb-launcher, bb-run, licenses/, ...). Its
#                 game/ is left out: the dump goes in the player's data folder.
#   CACHE_DIR     where the runtime and the license texts are kept after the download
#                 (default: build/appimage-cache; not $TMPDIR, which may be RAM).
# SOURCE_DATE_EPOCH sets the file times (default: the time of HEAD), so the same inputs give the
# same file. Needs curl, sha256sum and mksquashfs (package squashfs-tools, version 4.4 or later,
# with zstd). The runtime is never executed here.
set -euo pipefail

# The runtime: https://github.com/AppImage/type2-runtime, release 20251108 (built from commit
# dd6cebedcbddde9c82f89b011e8e1d40b6e43868). Statically linked, so the player needs no libfuse2,
# only fusermount3 and FUSE.
runtime_url=https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64
runtime_sha256=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d

# License texts of what the runtime links statically, each at a fixed tag or commit: name, URL,
# SHA-256. They go to licenses/appimage-runtime/ in the image and are not stored in this repository.
license_files=(
    "type2-runtime-MIT.txt https://raw.githubusercontent.com/AppImage/type2-runtime/dd6cebedcbddde9c82f89b011e8e1d40b6e43868/LICENSE aa154fc9070614bbe7921f89db11efd1dba7a1f3a41685958110e2230f9c0ca1"
    "libfuse-LGPL-2.1.txt https://raw.githubusercontent.com/libfuse/libfuse/fuse-3.15.0/LGPL2.txt dc626520dcd53a22f727af3ee42c770e56c97a64fe3adb063799d8ab032fe551"
    "squashfuse-BSD-2-Clause.txt https://raw.githubusercontent.com/vasi/squashfuse/0.5.2/LICENSE 9e909cc8a8ba27b1a649c964cf9eda37de911f3138c2b64e6d2f01703904ac13"
    "musl-MIT.txt https://git.musl-libc.org/cgit/musl/plain/COPYRIGHT?h=v1.2.5 f9bc4423732350eb0b3f7ed7e91d530298476f8fec0c6c427a1c04ade22655af"
    "zstd-BSD-3-Clause.txt https://raw.githubusercontent.com/facebook/zstd/v1.5.6/LICENSE 7055266497633c9025b777c78eb7235af13922117480ed5c674677adc381c9d8"
    "zlib.txt https://raw.githubusercontent.com/madler/zlib/v1.3.1/LICENSE 845efc77857d485d91fb3e0b884aaa929368c717ae8186b66fe1ed2495753243"
    "mimalloc-MIT.txt https://raw.githubusercontent.com/microsoft/mimalloc/v2.1.7/LICENSE 19c99805e7a44a34b297a75d1edea9985e300066dfc024d5c99d4236d4573b5d"
)

root=$(cd "$(dirname "$0")/../.." && pwd)
[[ $# -ge 2 ]] || { echo "usage: make-appimage.sh INSTALL_TREE OUTPUT_FILE [CACHE_DIR]" >&2; exit 2; }
tree=$(realpath "$1")
output=$(realpath -m "$2")
cache=$(realpath -m "${3:-$root/build/appimage-cache}")
[[ -x $tree/bb-launcher ]] || { echo "make-appimage: $tree/bb-launcher not found" >&2; exit 1; }
command -v mksquashfs >/dev/null || {
    echo "make-appimage: mksquashfs not found; install the package squashfs-tools (4.4 or later)" >&2
    exit 1
}
command -v curl >/dev/null || { echo "make-appimage: curl not found" >&2; exit 1; }

# Downloads URL to FILE unless FILE is there with the right hash. A wrong hash is an error.
fetch() {
    local file=$1 url=$2 sha=$3
    if [[ -f $file ]] && echo "$sha  $file" | sha256sum -c --quiet 2>/dev/null; then
        return
    fi
    rm -f "$file"
    curl -fsSL --retry 3 -o "$file.part" "$url"
    if ! echo "$sha  $file.part" | sha256sum -c --quiet; then
        echo "make-appimage: $url does not match its pinned SHA-256 $sha" >&2
        rm -f "$file.part"
        exit 1
    fi
    mv "$file.part" "$file"
}

mkdir -p "$cache/licenses"
fetch "$cache/runtime-x86_64" "$runtime_url" "$runtime_sha256"
for entry in "${license_files[@]}"; do
    read -r name url sha <<<"$entry"
    fetch "$cache/licenses/$name" "$url" "$sha"
done

epoch=${SOURCE_DATE_EPOCH:-$(git -C "$root" log -1 --format=%ct)}
work=$(mktemp -d "$cache/work.XXXXXX")
trap 'rm -rf "$work"' EXIT
appdir=$work/AppDir
mkdir "$appdir"
# The install tree without the dump folder, which the AppImage can't hold: the dump lives in the
# player's data folder.
tar -C "$tree" --exclude=./game -cf - . | tar -C "$appdir" -xf -
install -m 0755 "$root/packaging/AppRun" "$appdir/AppRun"
install -m 0644 "$root/packaging/bloodborne-linux.desktop" "$appdir/bloodborne-linux.desktop"
install -m 0644 "$root/packaging/bloodborne-linux.png" "$appdir/bloodborne-linux.png"
ln -s bloodborne-linux.png "$appdir/.DirIcon"
mkdir -p "$appdir/licenses/appimage-runtime"
install -m 0644 "$cache"/licenses/* "$appdir/licenses/appimage-runtime/"

# Fixed owner (-all-root: root owns every file, whoever builds it, so local builds are
# byte-identical), times and block size. mksquashfs reads
# SOURCE_DATE_EPOCH itself (for the image time, and as an upper bound for the file times, hence the
# touch). The runtime finds the squashfs right after its own ELF, so the two files are just joined.
find "$appdir" -exec touch -h -d "@$epoch" {} +
SOURCE_DATE_EPOCH=$epoch mksquashfs "$appdir" "$work/fs.squashfs" -comp zstd -b 128K -all-root \
    -noappend -no-xattrs -quiet >/dev/null
mkdir -p "$(dirname "$output")"
cat "$cache/runtime-x86_64" "$work/fs.squashfs" > "$output.part"
chmod 0755 "$output.part"
mv "$output.part" "$output"
echo "$output"
