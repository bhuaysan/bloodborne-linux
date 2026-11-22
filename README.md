# bloodborne-linux

An independent reverse engineering project with one goal: make the complete game of Bloodborne (PS4) playable natively on Linux, with no CPU emulation and the best performance and quality we can reach.

The PS4 runs x86-64 code, so the game's own machine code can run directly on a Linux PC. This project builds what is missing around it:

- a loader for the game executable
- reimplementations of the PS4 system libraries
- a translation layer from the PS4 graphics API to Vulkan

Game functions are decompiled into readable C++ only where that helps: to fix a bug, remove a bottleneck or lift a limit such as the frame rate. A complete decompilation is not a goal, so the game's own machine code keeps running for everything else.

The renderer, the shader translator and some system libraries are derived from the shadPS4 emulator, under the rules in [docs/source-policy.md](docs/source-policy.md). Every source is credited in [docs/references.md](docs/references.md).

## Status

`bb-run` plays the game. It goes from the title through character creation and the intro to the clinic, saves and continues, and plays Central Yharnam, fights included, at the game's 30 FPS. `bb-run --frame-rate 60` runs it at 60 and `--frame-rate 120` at 120 where the GPU thread keeps up (#676); the few differences from 30 that are left are listed in [docs/design/frame-rate.md](docs/design/frame-rate.md). A playthrough from start to an ending isn't checked yet (R7). See [docs/roadmap.md](docs/roadmap.md).

## Requirements

- Linux x86-64
- Your own legally obtained and dumped copy of Bloodborne (CUSA03173, patch 1.09)

**This repository contains no game files, assets or executables, and never will.** You need your own copy. Point the tools at it with:

```sh
export BB_GAME_DIR=/path/to/your/CUSA03173
```

## Playing

Download `Bloodborne-Linux-<version>-x86_64.tar.gz` from the [Releases page](https://github.com/bhuaysan/bloodborne-linux/releases), check it with `sha256sum -c --ignore-missing SHA256SUMS`, unpack it into a folder you can write and start `bb-launcher` in `Bloodborne-Linux/`. On the first start the launcher looks for your game folder, the one that holds `eboot.bin`. If it finds none, press "Set up game" and choose the folder of your own dump, or drag it onto the window. Then press Play. Everything the game writes stays in `Bloodborne-Linux/user/`. The `BB_GAME_DIR` setup above is for developers.

### Supported distributions

The package brings its own SDL, FFmpeg and C++ library. It takes the Vulkan driver, the display
libraries and the sound libraries from the system, which every desktop install has. Each line below
played the rig's route from a new game to first control in the clinic, with sound, in a container of
that distribution on an AMD GPU (RADV) under KDE Plasma on Wayland (`docs/test-rig.md`,
"Distributions").

| Distribution | Oldest version | What to install | Known limits |
|---|---|---|---|
| Fedora | 44 | nothing on a desktop | The reference machine |
| Ubuntu | VERSIONS_UBUNTU | `mesa-vulkan-drivers` (AMD and Intel) | UBUNTU_LIMITS |
| Debian | VERSIONS_DEBIAN | `mesa-vulkan-drivers` (AMD and Intel) | DEBIAN_LIMITS |
| Arch Linux | rolling | `vulkan-radeon` (AMD) or `vulkan-intel` | ARCH_LIMITS |
| Bazzite | stable image | nothing | BAZZITE_LIMITS |

A system without a desktop (a server or container image) also needs the libraries a desktop
brings: on Ubuntu and Debian `libxkbcommon0 libwayland-cursor0 libwayland-egl1 libxext6
libasound2t64 libpulse0 libpipewire-0.3-0t64 pipewire-bin libfontconfig1` (on Ubuntu 22.04 and
Debian 12 `libasound2` and `libpipewire-0.3-0`), on Arch `libxkbcommon libxext alsa-lib libpulse
pipewire fontconfig`. Without the display libraries the window does not open; without the sound ones the game plays silently. NVIDIA's driver and Intel GPUs
are not checked yet. The game needs fewer than 2000 memory mappings, so the older `vm.max_map_count`
default of 65530 is enough.

To build the same folder from source, run `tools/package-portable.sh` (see Building; the script names what it needs). Versions and how releases are made are in [docs/releasing.md](docs/releasing.md).

## Building

Install lines for Fedora, Ubuntu 24.04, Debian 13, Arch and Bazzite are in [docs/distributions.md](docs/distributions.md).

The runtime is C++20, built with CMake (3.27 or newer) and Ninja. The unit tests use GoogleTest, which the build finds as an installed package (on Fedora: `cmake ninja-build gtest-devel libasan libubsan`; on Debian and Ubuntu: `cmake ninja-build libgtest-dev`).

```sh
cmake --preset debug          # or relwithdebinfo, release
cmake --build --preset debug
ctest --preset debug
```

| Preset | Use |
|---|---|
| `debug` | everyday development, with AddressSanitizer and UndefinedBehaviorSanitizer |
| `relwithdebinfo` | optimized and debuggable, for playtesting and profiling |
| `release` | optimized |

Each preset builds into `build/<preset>/`. Tests that need the game skip when `BB_GAME_DIR` is not set.

`bb-load` checks that your dump loads: it maps the game executable and the two bundled PRXs it loads at start (`libSceFios2.prx`, `libc.prx`), binds their imports, then prints their segments, TLS templates and imports, without running any game code. It takes the game from `BB_GAME_DIR` or `--game-dir`:

```sh
build/debug/src/tools/bb-load --game-dir /path/to/your/CUSA03173
```

CI (GitHub Actions, `.github/workflows/ci.yml`) builds the presets in a Fedora 44 container (all three on `main`, `debug` and `release` on pull requests) with warnings as errors, runs the tests and checks every commit of a pull request with `tools/privacy-check.sh`. CI has no game files, so the game tests skip there.

## Documentation

- [Architecture](docs/architecture.md)
- [Roadmap](docs/roadmap.md)
- [Releases](docs/releasing.md)
- [Source and license policy](docs/source-policy.md)
- [References and credits](docs/references.md)

## Contributing

Before your first commit, enable the repository's git hooks:

```sh
git config core.hooksPath tools/githooks
```

## License

Copyright (C) 2026 bloodborne-linux contributors. The project is licensed under the GNU General Public License, version 2 or (at your option) any later version (see [LICENSE](LICENSE)). Code taken from other projects keeps its own license and notices (`third_party/`, header comments). Code written before 2026-11-06 was published under the MIT license. The license does not cover the game.

## Legal

Bloodborne is a trademark of Sony Interactive Entertainment. This project is not affiliated with or endorsed by Sony Interactive Entertainment or FromSoftware.
