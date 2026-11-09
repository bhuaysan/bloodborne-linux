# Roadmap

The main goal is the complete game, playable from start to end, with the best performance and quality we can reach. A complete decompilation is not a goal.

Each milestone matches a GitHub milestone. Tasks are tracked as issues.

The work runs on three tracks that overlap after M2:

- **Runtime:** loader and OS layer, until the game boots.
- **Decomp:** game functions, verified with differential tests, only where they serve playability, performance or quality. Can start as soon as the loader can map the executable.
- **Graphics:** GNM→Vulkan and GCN→SPIR-V. A long-running track. The first frame (M4.5) and playtesting wait for it.

| Milestone | Goal | Done when |
|---|---|---|
| **M0 Setup** | Repo, docs, tooling, guardrails | Hooks work, Ghidra is set up, first push done |
| **M1 Executable analysis** | Understand the executable | Our own SELF→ELF extractor checks the game hash and produces an ELF that Ghidra imports cleanly, using our own preprocessor/loader script. Segments, modules, imports and NIDs are listed and documented in `docs/formats/`. The NID name-matching tool works. Symbol export/import scripts sync Ghidra with `symbols/` |
| **M2 Loader + test harness** | Map the executable; verify decomp | The ELF maps and relocates in a Linux process, and imports resolve to logging stubs. A differential test harness calls an original function and its C++ replacement with the same inputs and compares the results. The first functions are decompiled and pass. CMake presets (`debug` with sanitizers, `relwithdebinfo`, `release`) and GitHub Actions CI (build, tests, privacy check) are in place |
| **M3 Core OS** | Reach the main loop | The game-bundled `libc.prx` and `libSceFios2.prx` load like the executable. Our firmware-library implementations (threads, memory, sync, time, file I/O) carry the game to its main loop. Runtime output goes to XDG directories: logs under `$XDG_STATE_HOME`, save data under `$XDG_DATA_HOME` per `--profile`. Caches and config come later |
| **M4.0 Translator and GPU foundations** | The parts the first frame builds on | GCN→SPIR-V translator core, `bbvk` device core, PM4 parser and GPU thread, tiling, render targets. The closed issues of the original M4 |
| **M4.1 Command streams** | The GPU command stream runs completely | A headless game run has no failed streams, end-of-pipe interrupts reach registered equeue events (import test; the game registers none), and `--gpu-trace` has written a trace of the first frames |
| **M4.2 Memory mirror and compute** | The GPU reads guest memory | The dispatches of the game run through the GPU memory mirror |
| **M4.3 Synthetic draw** | A draw works end to end | A synthetic PM4 stream renders a triangle on lavapipe through the whole path: parser, draw snapshot, pipeline, mirror, draw, read-back |
| **M4.4 Textures and shader gaps** | What the first frame needs | A synthetic textured draw passes and every shader in the first-frame trace translates |
| **M4.5 First frame** | Show something on screen | The first frame of the game appears in the window |
| **M4.6 Remaining shader stages** | The rest of the shader corpus | ES, GS, LS, HS and domain shaders, scratch memory and the shader features the first frame did not need |
| **M5 Input & audio** | Playable basics | Controller input and audio output work |
| **M6 Playthrough** | The complete game is playable | The game can be played from the start to an ending on the reference machine, without a crash or a rendering fault that blocks play. The work goes area by area, and every fault found gets an issue |
| **M7 Performance and quality** | The best version of the game | Frame times stay even at the game's original frame rate on the reference machine. Shaders and pipelines are cached on disk, so a second visit to an area doesn't stutter. A resolution and a frame rate above the PS4's can be set and run without gameplay faults |
| **Decomp** (continuous from M2, as needed) | Replace game code where that helps | No end state of its own. A function is decompiled when an issue needs it for a bug, a crash, a bottleneck or a limit such as the frame rate. It passes its differential test and is hooked in. Progress tracked in `symbols/functions.csv` |

The maintainer tags each completed milestone (`m1`, `m2`, …) after its review.

M4 was split into M4.0 to M4.6 on 2026-11-08, because its only completion criterion was the first frame and its issues kept needing several PRs each. From M4.1 on, one issue is one PR, each part ends with something that can be checked, and a part ends only when all its work has landed.
