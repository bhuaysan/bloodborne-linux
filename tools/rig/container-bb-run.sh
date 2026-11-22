#!/usr/bin/env bash
# Runs a package's bb-run inside a container of another distribution, for the rig's --bb-run
# (docs/test-rig.md, "Distributions"). The container gets what distrobox shares with the host (the
# home folder, the runtime folder with the Wayland, PipeWire and PulseAudio sockets, X11, the GPU,
# the host's network and IPC, your own user id) and the game folder read-only, but none of the
# packages distrobox installs into it, so the image decides what the package finds.
#
#   BB_CONTAINER_IMAGE=<image> BB_PACKAGE_DIR=<unpacked Bloodborne-Linux/> \
#     tools/rig/rig.py run bb-run SCENARIO --window --bb-run "$PWD/tools/rig/container-bb-run.sh"
#
# The container stays in the caller's cgroup (--cgroups=disabled), so the rig's memory limit,
# samplers and stop reach bb-run inside it. The environment is passed through (--env-host).
# Image-based systems (Bazzite) link /home to var/home but ship no /var/home: add it in the image.
set -euo pipefail
: "${BB_CONTAINER_IMAGE:?set BB_CONTAINER_IMAGE to the container image}"
: "${BB_PACKAGE_DIR:?set BB_PACKAGE_DIR to the unpacked package folder}"
# The game folder bb-run is given (the rig passes --game-dir), else BB_GAME_DIR, read-only inside.
game=${BB_GAME_DIR:-}
previous=
for arg in "$@"; do
    [ "$previous" = --game-dir ] && game=$arg
    previous=$arg
done
[ -n "$game" ] || { echo "container-bb-run: no --game-dir and no BB_GAME_DIR" >&2; exit 2; }
package=$(realpath -e "$BB_PACKAGE_DIR")
exec podman run --rm --cgroups=disabled --userns=keep-id --security-opt label=disable \
    --network host --ipc host --device /dev/dri \
    -v "$HOME:$HOME" -v "$package:$package:ro" -v "$game:$game:ro" \
    -v "$XDG_RUNTIME_DIR:$XDG_RUNTIME_DIR" -v /tmp/.X11-unix:/tmp/.X11-unix \
    --env-host -e HOME="$HOME" -w "$PWD" \
    "$BB_CONTAINER_IMAGE" "$package/bb-run" "$@"
