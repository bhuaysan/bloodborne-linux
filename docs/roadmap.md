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

M4.4 to M4.6 and the old M5 to M7 are dropped. R1 closes or moves their open issues.

## Plan

Each step is a GitHub milestone, and tasks are tracked as issues.

| Milestone | Goal | Done when |
|---|---|---|
| **R1 Rules** | The new plan is written down | `AGENTS.md`, this roadmap, `docs/source-policy.md` and `docs/architecture.md` describe it. The open issues of the old milestones are closed or moved |
| **R2 Test rig** | Compare our program with the reference | One command runs the reference or our program with scripted input, a time limit, a memory limit and a read-only game directory, and writes logs, screenshots and per-frame times to `analysis/`. The reference run from the main menu through character creation and the intro to the clinic can be repeated |
| **R3 Shader translator** | Translate every shader | `shader_recompiler` from shadPS4 v.0.21.0 runs in our tree. Each of the 3,832 distinct programs in the corpus translates and passes `spirv-val`, or is listed with a reason. `src/gpu/gcn` is gone |
| **R4 Renderer** | Our program draws the game | `video_core` runs behind a thin host layer (guest memory, write tracking, flips, window). `bb-run` shows the main menu, and its frame matches the reference. `src/gpu/pm4` and the memory mirror are gone |
| **R5 OS gaps** | The game is playable in our program | Pad, audio with ATRAC9, video playback and dialogs work. Under scripted input our program goes from the menu through a new game to the clinic, saves and continues, and runs 25 minutes without a crash, like the reference |
| **R6 Rewrite** | Fit the derived code to this port | Emulator structure a single-game port doesn't need is gone: settings objects, debug state, singletons, paths for other games. Each rewrite is checked against reference frames, frame times and the shader corpus. Whatever stays in shadPS4's shape stays on purpose, and the reason is written down. Can overlap R7 |
| **R7 Playthrough** | The complete game is playable | The game can be played from the start to an ending on the reference machine, without a crash or a rendering fault that blocks play. The work goes area by area, and every fault found gets an issue |
| **R8 Performance and quality** | The best version of the game | Frame times stay even at the game's original frame rate on the reference machine. Shaders are translated ahead of time and pipelines are cached on disk, so a second visit to an area doesn't stutter. A resolution and a frame rate above the PS4's can be set and run without gameplay faults. Engine render functions are replaced with hooks where that measurably helps |
| **Decomp** (continuous, as needed) | Replace game code where that helps | No end state of its own. A function is decompiled when an issue needs it for a bug, a crash, a bottleneck or a limit such as the frame rate. It passes its differential test and is hooked in. Progress is tracked in `symbols/functions.csv` |

The maintainer tags each completed milestone after its review.

## The reference run

The official shadPS4 v.0.21.0 Linux release on the reference machine (RADV on an RX 7800 XT), 2026-11-09, without firmware files, with a read-only game directory and input from a virtual gamepad:

- It went through the main menu, character creation with the name dialog, the intro and clinic gameplay, then saved and continued.
- It ran 25 minutes without a crash and without an amdgpu fault in the kernel log.
- It held 30 FPS, the game's cap, in the menu and the clinic, with 5.6 GB peak memory.

Not measured yet: combat, outdoor areas, sound and per-frame times. R2 adds the measurements.
