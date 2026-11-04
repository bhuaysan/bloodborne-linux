# bloodborne-linux

A from-scratch reverse engineering project with one goal: run Bloodborne (PS4) natively on Linux, without emulation.

The PS4 runs x86-64 code, so the game's own machine code can run directly on a Linux PC. This project builds what is missing around it:

- a loader for the game executable
- reimplementations of the PS4 system libraries
- a translation layer from the PS4 graphics API to Vulkan

On top of that, game functions are gradually decompiled into readable C++ until the original executable is no longer needed.

This is also a learning project. Everything here is worked out independently.

## Status

Early setup. Nothing runs yet. See [docs/roadmap.md](docs/roadmap.md).

## Requirements

- Linux x86-64
- Your own legally obtained and dumped copy of Bloodborne (CUSA03173, patch 1.09)

**This repository contains no game files, assets or executables, and never will.** You need your own copy. Point the tools at it with:

```sh
export BB_GAME_DIR=/path/to/your/CUSA03173
```

## Documentation

- [Architecture](docs/architecture.md)
- [Roadmap](docs/roadmap.md)
- [References and credits](docs/references.md)

## Contributing

Before your first commit, enable the repository's git hooks:

```sh
git config core.hooksPath tools/githooks
```

## Legal

Bloodborne is a trademark of Sony Interactive Entertainment. This project is not affiliated with or endorsed by Sony Interactive Entertainment or FromSoftware.
