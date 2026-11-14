# Roadmap

The main goal is the complete game, playable from start to end, with the best performance and quality we can reach. A complete decompilation is not a goal.

On 2026-11-09 the plan changed. M0 to M4.3 gave us a loader and an OS layer that carry the game to its main loop, and a GPU path that never drew a frame of the game. shadPS4 already runs the game on the reference machine. So we stop rebuilding its renderer and shader translator and derive them from it instead. See [architecture.md](architecture.md) for what that means for the design.

## Done

| Milestone | Result |
|---|---|
| **M0 Setup** | Repo, docs, tooling, guardrails, Ghidra |
| **M1 Executable analysis** | SELF→ELF extractor with hash check, imports and NIDs documented, NID name matching, Ghidra sync with `symbols/` |
| **M2 Loader + test harness** | The executable maps and relocates, differential test harness, first functions decompiled, CMake presets and CI |
| **M3 Core OS** | The game's bundled `libc.prx` and `libSceFios2.prx` load, and our libraries carry the game to its main loop |
| **M4.0 to M4.3 GPU foundations** | Our own translator, GPU thread, memory mirror and draw path. R3 and R4 replace them |
| **R1 Rules** | `AGENTS.md`, this roadmap, `docs/source-policy.md` and `docs/architecture.md` describe the plan of 2026-11-09, and the open issues of the old milestones are closed or moved |
| **R2 Test rig** | `tools/rig/rig.py` runs the reference or `bb-run` with scripted input, a time limit, a memory limit and a read-only game directory, and repeats the reference run from a fresh data directory to the clinic |
| **R3 Shader translator** | shadPS4's `shader_recompiler` runs in our tree and translates all 3,832 programs of the corpus to valid SPIR-V, except nine that are listed by hash with their cause |
| **R4 Renderer** | `video_core` runs behind a thin host layer, `bb-run` shows the main menu and matches the reference frame, and `src/gpu` and the memory mirror are gone |
| **R5 OS gaps** | Pad, audio with ATRAC9, movies and the name dialog work: `bb-run` goes through a new game to the clinic, saves and continues, and plays 25 minutes without a crash or GPU fault, matching the reference |

R1 has no tag and no milestone review: it changed only rules and docs. R2 was reviewed and tagged after R5. M4.4 to M4.6 and the old M5 to M7 are dropped, and R1 closed or moved their open issues.

## Plan

Each step is a GitHub milestone, and tasks are tracked as issues.

| Milestone | Goal | Done when |
|---|---|---|
| **R6 Rewrite** | Fit the derived code to this port | Emulator structure a single-game port doesn't need is gone: settings objects, debug state, singletons, paths for other games. Each rewrite is checked against reference frames, frame times and the shader corpus. Whatever stays in shadPS4's shape stays on purpose, and the reason is written down. Can overlap R7 |
| **R7 Playthrough** | The complete game is playable | The game can be played from the start to an ending on the reference machine, without a crash or a rendering fault that blocks play. The work goes area by area, and every fault found gets an issue |
| **R8 Performance and quality** | The best version of the game | Frame times stay even at the game's original frame rate on the reference machine. Translated shaders and pipelines are cached on disk, so a second visit to an area doesn't stutter, and building them doesn't stall the frame on a first visit (translating ahead of time from the archives can't match the keys the game looks up: see #512 in `docs/design/shader-corpus.md`). A resolution and a frame rate above the PS4's can be set and run without gameplay faults. Engine render functions are replaced with hooks where that measurably helps |
| **Decomp** (continuous, as needed) | Replace game code where that helps | No end state of its own. A function is decompiled when an issue needs it for a bug, a crash, a bottleneck or a limit such as the frame rate. It passes its differential test and is hooked in. Progress is tracked in `symbols/functions.csv` |

`src/gpu` was removed in R4 (#435), with the memory mirror, `bb-gcn` and the options of the old path (maintainer decision, 2026-11-09).

The maintainer tags each completed milestone after its review.

## The reference run

The official shadPS4 v.0.21.0 Linux release on the reference machine (RADV on an RX 7800 XT), 2026-11-09, without firmware files, with a read-only game directory and input from a virtual gamepad:

- It went through the main menu, character creation with the name dialog, the intro and clinic gameplay, then saved and continued.
- It ran 25 minutes without a crash and without an amdgpu fault in the kernel log.
- It held 30 FPS, the game's cap, in the menu and the clinic, with 5.6 GB peak memory.

The test rig (`docs/test-rig.md`) repeats the run from a fresh data directory to first control in the clinic. In its last five runs (2026-11-09) the median frame interval was 33.33 ms and the 99th percentile about 50 ms, with about 4.1 GB of program memory at peak.

Not measured yet: combat, outdoor areas and sound.
