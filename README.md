# bloodborne-linux

A from-scratch reverse engineering project with one goal: run Bloodborne (PS4) natively on Linux, without emulation.

The PS4 runs x86-64 code, so the game's own machine code can run directly on a Linux PC. This project builds what is missing around it:

- a loader for the game executable
- reimplementations of the PS4 system libraries
- a translation layer from the PS4 graphics API to Vulkan

On top of that, game functions are gradually decompiled into readable C++ until the original executable is no longer needed.

This is also a learning project. Everything here is worked out independently.

## Status

Executable analysis (M1) and the loader with its test harness (M2) are done. `bb-load` maps and relocates the game executable in a Linux process, imports resolve to logging stubs, and the first game functions are decompiled to C++ and pass differential tests against the original. No game code runs on its own yet; reaching the main loop is next (M3). See [docs/roadmap.md](docs/roadmap.md).

## Requirements

- Linux x86-64
- Your own legally obtained and dumped copy of Bloodborne (CUSA03173, patch 1.09)

**This repository contains no game files, assets or executables, and never will.** You need your own copy. Point the tools at it with:

```sh
export BB_GAME_DIR=/path/to/your/CUSA03173
```

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

`bb-load` checks that your dump loads: it maps the game executable, then prints its segments, TLS template and imports, without running any game code. It takes the game from `BB_GAME_DIR` or `--game-dir`:

```sh
build/debug/src/tools/bb-load --game-dir /path/to/your/CUSA03173
```

CI (GitHub Actions, `.github/workflows/ci.yml`) builds all three presets on Ubuntu 24.04 with warnings as errors, runs the tests and checks every commit of a pull request with `tools/privacy-check.sh`. CI has no game files, so the game tests skip there.

## Documentation

- [Architecture](docs/architecture.md)
- [Roadmap](docs/roadmap.md)
- [Clean-room policy](docs/clean-room.md)
- [References and credits](docs/references.md)

## Contributing

Before your first commit, enable the repository's git hooks:

```sh
git config core.hooksPath tools/githooks
```

## License

The project's own code and documentation are MIT licensed (see [LICENSE](LICENSE)). The license does not cover the game.

## Legal

Bloodborne is a trademark of Sony Interactive Entertainment. This project is not affiliated with or endorsed by Sony Interactive Entertainment or FromSoftware.
