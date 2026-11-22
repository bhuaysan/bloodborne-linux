#!/usr/bin/env bash
# Starts the portable package on a distribution, in a fresh container of it: CI runs it for every
# distribution R12 supports (ci.yml, job distro-check; docs/distributions.md, "Checking a system").
#
# Usage: tools/distro-check.sh [--leave-out PACKAGE] <distro> <package.tar.gz>
#        distro: fedora44 ubuntu2204 ubuntu2404 debian12 debian13 arch bazzite
#
# The container installs only the Vulkan loader, lavapipe and fontconfig with the distribution's
# own package manager (Bazzite's image has them already), unpacks the package, runs ldd on bb-run
# and bb-launcher (no "not found") and `bb-run --check-system --headless` on lavapipe. With
# --leave-out the named package of the list is not installed, and the script succeeds only when
# the check fails: it shows that a missing library is noticed. Needs podman (or docker) and a
# network for the image and the packages.
set -euo pipefail

usage() { sed -n '5,6p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }

leave_out=
while [ $# -gt 2 ]; do
    case $1 in
        --leave-out) leave_out=$2; shift 2 ;;
        *) usage ;;
    esac
done
[ $# -eq 2 ] || usage
distro=$1
package=$(realpath -e "$2")

# Image, package manager command and the packages: Vulkan loader, lavapipe, fontconfig.
case $distro in
    fedora44) image=docker.io/library/fedora:44 ;;
    ubuntu2204) image=docker.io/library/ubuntu:22.04 ;;
    ubuntu2404) image=docker.io/library/ubuntu:24.04 ;;
    debian12) image=docker.io/library/debian:12 ;;
    debian13) image=docker.io/library/debian:13 ;;
    arch) image=docker.io/library/archlinux:latest ;;
    bazzite) image=ghcr.io/ublue-os/bazzite:stable ;;
    *) usage ;;
esac
case $distro in
    fedora*) install="dnf -y --setopt=install_weak_deps=False install"
             packages="vulkan-loader mesa-vulkan-drivers fontconfig" ;;
    ubuntu* | debian*) install="apt-get update && apt-get install -y --no-install-recommends"
                       packages="libvulkan1 mesa-vulkan-drivers libfontconfig1" ;;
    arch) install="pacman -Sy --noconfirm --needed"
          packages="vulkan-icd-loader vulkan-swrast fontconfig" ;;
    # An image of the installed system: it ships them, and its package database is read-only.
    bazzite) install=; packages="vulkan-loader mesa-vulkan-drivers fontconfig" ;;
esac
if [ -n "$leave_out" ]; then
    case " $packages " in
        *" $leave_out "*) ;;
        *) echo "distro-check: --leave-out $leave_out is not one of: $packages" >&2; exit 2 ;;
    esac
    [ -n "$install" ] || { echo "distro-check: $distro installs nothing to leave out" >&2; exit 2; }
    packages=$(printf '%s\n' $packages | grep -vxF -- "$leave_out" | tr '\n' ' ')
fi
if [ -n "$install" ]; then
    install_step="$install $packages"
else
    install_step="rpm -q $packages"
fi

if command -v podman >/dev/null; then
    engine=podman
elif command -v docker >/dev/null; then
    engine=docker
else
    echo "distro-check: neither podman nor docker found" >&2
    exit 1
fi

# The script that runs inside the container. Each step prints a "== " header, so the log shows
# where it stopped. A failed install exits 3, a failed check 1.
inner=$(cat <<EOS
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
echo "== install: $install_step"
$install_step >/tmp/install.log 2>&1 || { cat /tmp/install.log; exit 3; }
grep -m1 PRETTY_NAME /etc/os-release
echo "== unpack"
mkdir /tmp/bb
tar -xzf /package.tar.gz -C /tmp/bb
cd /tmp/bb/Bloodborne-Linux
status=0
for bin in bb-run bb-launcher; do
    echo "== ldd \$bin"
    if ! deps=\$(ldd "\$bin" 2>&1); then
        echo "\$deps"; status=1
    elif grep 'not found' <<<"\$deps"; then
        status=1
    fi
done
[ "\$status" -eq 0 ] || exit 1
echo "== bb-run --check-system --headless"
VK_LOADER_DRIVERS_SELECT='*lvp*' BB_USER_DIR=/tmp/bb-user ./bb-run --check-system --headless
EOS
)

status=0
"$engine" run --rm -v "$package:/package.tar.gz:ro,Z" "$image" bash -c "$inner" || status=$?
if [ -n "$leave_out" ]; then
    # 1 is the check's failure; anything else (the install, the container) proves nothing.
    if [ "$status" -ne 1 ]; then
        echo "== distro-check $distro: status $status without $leave_out, expected the check's 1" >&2
        exit 1
    fi
    echo "== distro-check $distro: failed without $leave_out, as expected"
    exit 0
fi
echo "== distro-check $distro: $([ "$status" -eq 0 ] && echo passed || echo "failed ($status)")"
exit "$status"
