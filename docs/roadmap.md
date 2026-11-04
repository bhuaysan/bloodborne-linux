# Roadmap

Each milestone matches a GitHub milestone. Tasks are tracked as issues.

The work runs on three tracks that overlap after M2:

- **Runtime:** loader and OS layer, until the game boots.
- **Decomp:** game functions, verified with differential tests. Can start as soon as the loader can map the executable.
- **Graphics:** GNM→Vulkan and GCN→SPIR-V. A long-running track. Nothing else waits for it.

| Milestone | Goal | Done when |
|---|---|---|
| **M0 Setup** | Repo, docs, tooling, guardrails | Hooks work, Ghidra is set up, first push done |
| **M1 Executable analysis** | Understand the executable | Our own SELF→ELF extractor checks the game hash and produces an ELF that Ghidra imports cleanly, using our own preprocessor/loader script. Segments, modules, imports and NIDs are listed and documented in `docs/formats/`. The NID name-matching tool works. Symbol export/import scripts sync Ghidra with `symbols/` |
| **M2 Loader + test harness** | Map the executable; verify decomp | The ELF maps and relocates in a Linux process, and imports resolve to logging stubs. A differential test harness calls an original function and its C++ replacement with the same inputs and compares the results. The first functions are decompiled and pass. CMake presets (`debug` with sanitizers, `relwithdebinfo`, `release`) and GitHub Actions CI (build, tests, privacy check) are in place |
| **M3 Core OS** | Reach the main loop | The game-bundled `libc.prx` and `libSceFios2.prx` load like the executable. Our firmware-library implementations (threads, memory, sync, time, file I/O) carry the game to its main loop. Runtime output (saves, caches, logs, config) goes to XDG directories, with `--profile` for separate save data |
| **M4 Graphics** (long-running) | Show something on screen | PM4 parsing, GPU state tracking, GCN→SPIR-V translation; the first frame renders |
| **M5 Input & audio** | Playable basics | Controller input and audio output work |
| **Decomp** (continuous from M2) | Replace game code | Functions are decompiled, pass differential tests and are hooked in. Progress tracked in `symbols/functions.csv` |

The maintainer tags each completed milestone (`m1`, `m2`, …) after its review.
