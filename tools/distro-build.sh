#!/usr/bin/env bash
# Builds the release preset from source on a distribution's own packages, in a fresh rootless
# podman container, and runs the unit tests. CI calls this once per distribution.
#
# Usage: tools/distro-build.sh [--keep] [--jobs N] <fedora|ubuntu|debian|arch>
#        tools/distro-build.sh --install-line <fedora|ubuntu|debian|arch>
#
# The container installs the packages listed in tools/distro-packages/<distro>.txt (the same list
# docs/distributions.md shows as the install line), configures with warnings as errors, builds
# and runs ctest. The source goes in as a tree copy of the tracked and new files (the build never
# writes into the checkout); the build tree lives in a directory outside the repository,
# $BB_DISTRO_WORK (default: a new directory under $TMPDIR), removed afterwards unless --keep.
# Tests that need the game skip: the container has no game files.
#
# Prints the GCC version and the build time at the end. Needs network (packages, the pinned
# downloads of cmake/*.cmake) and podman.
set -euo pipefail

usage() { sed -n '2,3p;5p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }

root=$(cd "$(dirname "$0")/.." && pwd)
lists=$root/tools/distro-packages

# One line per package, without the comments.
packages() { grep -v '^[[:space:]]*\(#\|$\)' "$lists/$1.txt"; }

# The command that installs a distribution's packages.
install_line() {
    local pkgs; pkgs=$(packages "$1" | tr '\n' ' '); pkgs=${pkgs% }
    case $1 in
        fedora) echo "sudo dnf install $pkgs" ;;
        ubuntu | debian) echo "sudo apt install $pkgs" ;;
        arch) echo "sudo pacman -S --needed $pkgs" ;;
    esac
}

image_of() {
    case $1 in
        fedora) echo docker.io/library/fedora:44 ;;
        ubuntu) echo docker.io/library/ubuntu:24.04 ;;
        debian) echo docker.io/library/debian:13 ;;
        arch) echo docker.io/library/archlinux:latest ;;
        *) usage ;;
    esac
}

keep=0
jobs=${CMAKE_BUILD_PARALLEL_LEVEL:-6}
mode=build
while [ $# -gt 1 ]; do
    case $1 in
        --keep) keep=1; shift ;;
        --jobs) jobs=$2; shift 2 ;;
        --install-line) mode=line; shift ;;
        *) usage ;;
    esac
done
[ $# -eq 1 ] || usage
distro=$1
image=$(image_of "$distro")
[ -f "$lists/$distro.txt" ] || usage
if [ "$mode" = line ]; then install_line "$distro"; exit 0; fi

command -v podman >/dev/null || { echo "distro-build: podman not found" >&2; exit 1; }
work=${BB_DISTRO_WORK:-$(mktemp -d "${TMPDIR:-/tmp}/bb-distro-$distro.XXXXXX")}
mkdir -p "$work"
if [ "$keep" -eq 0 ]; then trap 'rm -rf "$work"' EXIT; fi
rm -rf "$work/src" "$work/build"; mkdir -p "$work/src"
cd "$root"
{ git ls-files -z; git ls-files -z --others --exclude-standard; } |
    tar --null -T - --ignore-failed-read -cf - | tar -xf - -C "$work/src"
case $distro in
    fedora) install_cmd="dnf -y --setopt=install_weak_deps=False install" ;;
    ubuntu | debian) install_cmd="apt-get install -y --no-install-recommends" ;;
    arch) install_cmd="pacman -Syu --noconfirm --needed" ;;
esac
# Ubuntu 24.04's default GCC 13 can't build the shader translator (CMakeLists.txt).
compiler_env=
[ "$distro" != ubuntu ] || compiler_env="export CC=gcc-14 CXX=g++-14"
case $distro in
    ubuntu | debian) prepare="export DEBIAN_FRONTEND=noninteractive; apt-get update" ;;
    *) prepare=true ;;
esac

# The script that runs inside the container.
inner=$(cat <<EOS
set -euo pipefail
$compiler_env
$prepare
$install_cmd $(packages "$distro" | tr '\n' ' ')
echo "== compiler"; "\${CXX:-g++}" --version | head -1
cd /work/src
start=\$(date +%s)
cmake --preset release -B /work/build -DBB_WERROR=ON
cmake --build /work/build
echo "BUILD_SECONDS=\$((\$(date +%s) - start))"
ctest --test-dir /work/build --output-on-failure --no-tests=error
EOS
)
log=$work/log.txt
status=0
# Memory-limited, no swap: the machine has other work.
podman run --rm --memory 8g --memory-swap 8g -e CMAKE_BUILD_PARALLEL_LEVEL="$jobs" \
    -e CTEST_PARALLEL_LEVEL="$jobs" -v "$work:/work:rw,Z" "$image" bash -c "$inner" 2>&1 |
    tee "$log" || status=$?
echo "== distro-build $distro: $(grep -A1 -m1 '^== compiler' "$log" | tail -1); $(grep -m1 BUILD_SECONDS "$log" || echo 'no build time')"
exit "$status"
