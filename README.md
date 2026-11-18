# bloodborne-linux

An independent reverse engineering project with one goal: make the complete game of Bloodborne (PS4) playable natively on Linux, with no CPU emulation and the best performance and quality we can reach.

The PS4 runs x86-64 code, so the game's own machine code can run directly on a Linux PC. This project builds what is missing around it:

- a loader for the game executable
- reimplementations of the PS4 system libraries
- a translation layer from the PS4 graphics API to Vulkan

Game functions are decompiled into readable C++ only where that helps: to fix a bug, remove a bottleneck or lift a limit such as the frame rate. A complete decompilation is not a goal, so the game's own machine code keeps running for everything else.

The renderer, the shader translator and some system libraries are derived from the shadPS4 emulator, under the rules in [docs/source-policy.md](docs/source-policy.md). Every source is credited in [docs/references.md](docs/references.md).

## Status

`bb-run` plays the game. It goes from the title through character creation and the intro to the clinic, saves and continues, and plays Central Yharnam, fights included, at the game's 30 FPS. `bb-run --frame-rate 60` runs it at 60; the few differences from 30 that are left are listed in [docs/design/frame-rate.md](docs/design/frame-rate.md). A playthrough from start to an ending isn't checked yet (R7). See [docs/roadmap.md](docs/roadmap.md).

## Requirements

- Linux x86-64
- Your own legally obtained and dumped copy of Bloodborne (CUSA03173, patch 1.09)

**This repository contains no game files, assets or executables, and never will.** You need your own copy. Point the tools at it with:

```sh
export BB_GAME_DIR=/path/to/your/CUSA03173
```

## Playing

Build the `release` preset (see Building), install it and start the launcher:

```sh
cmake --preset release
cmake --build --preset release
cmake --install build/release --prefix ~/.local   # or /usr/local with sudo
```

"Bloodborne on Linux" now shows in your application menu (log out and in if it doesn't at once), or run `~/.local/bin/bb-launcher`. On the first start the launcher looks for your game folder, the one that holds `eboot.bin`. If it finds none, press "Set up game" and choose the folder of your own dump, or drag it onto the window. Then press Play. The `BB_GAME_DIR` setup above is for developers.

## Building

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
