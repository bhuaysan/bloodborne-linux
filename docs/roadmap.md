# Roadmap

The main goal is the complete game, playable from start to end, with the best performance and quality we can reach. A complete decompilation is not a goal.

On 2026-11-09 the plan changed. M0 to M4.3 gave us a loader and an OS layer that carry the game to its main loop, and a GPU path that never drew a frame of the game. shadPS4 already ran the game on the reference machine, so we derived its renderer and shader translator instead of rebuilding them. See [architecture.md](architecture.md) for what that means for the design.

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
| **R6 Rewrite** | The emulator structure a single-game port doesn't need is gone: settings objects, debug state, singletons, other platforms. What stays in shadPS4's shape stays on purpose, with the reason in `docs/design/rewrite.md` |
| **R8 Performance and quality** | Shaders and pipelines are cached on disk and built off the GPU thread (R8.1). Frame times stay even on the clinic and Central Yharnam routes (R8.2). `bb-run --frame-rate 60` runs the game at normal speed: the frame pacer, the main loop tick and 14 other game functions are replaced, and every duration of the final check matches 30 FPS within one 30 FPS frame (R8.3, `docs/design/frame-rate.md`). `bb-run --resolution` renders up to 3840x2160 through the engine's own frame buffer size and a larger graphics heap (R8.4, `docs/design/resolution.md`) |
| **R9 Launcher** | `bb-launcher` sets up the game folder of the player's dump (by search, drop or dialog, with the `eboot.bin` hash check), sets picture and save profile in the shared config file, starts `bb-run --config` and appears in the application menu |
| **R10 Performance** | `bb-run --frame-rate 120` runs the game at normal speed, every duration of the R8.3 check matching 30 FPS within one 30 FPS frame. Draw commands are recorded on a thread of their own, and the clinic frame holds 8.33 ms at 120 (`docs/design/command-recording.md`). Loads take as long as the reference's (`docs/test-rig.md`, "Load times (#655)"), and the frame pacer sleeps before it spins |
| **R11 Image quality** | Anisotropic filtering, switches for motion blur and chromatic aberration, a field of view, and 21:9 and 32:9 with the HUD at 16:9 are options of `bb-run` and the launcher's picture page, and the defaults leave the picture as it is (`docs/design/image-quality.md`). Larger shadow maps (#718) and longer draw distances (#720) were researched; their options wait until a spot is found where they are visible |

R1 has no tag and no milestone review: it changed only rules and docs. R2 was reviewed and tagged after R5. M4.4 to M4.6 and the old M5 to M7 are dropped, and R1 closed or moved their open issues.

## Plan

Each step is a GitHub milestone, and tasks are tracked as issues.

| Milestone | Goal | Done when |
|---|---|---|
| **R7 Playthrough** | The complete game is playable | The game can be played from the start to an ending on the reference machine, without a crash or a rendering fault that blocks play. The work goes area by area, and every fault found gets an issue |
| **R12 Distributions** | The game runs on the common Linux distributions | The portable package runs and plays on Fedora, Ubuntu, Debian, Arch and Bazzite, and the source builds on them (`docs/distributions.md`) |
| **R13 Heavy-area performance** | 120 FPS in the heaviest areas | Every segment of the heaviest areas that misses 8.33 ms at 120 FPS (#654) holds it, or has a measured bound outside the GPU thread (the main loop or the GPU) with an issue of its own. 30 and 60 FPS stay as they are |
| **Decomp** (continuous, as needed) | Replace game code where that helps | No end state of its own. A function is decompiled when an issue needs it for a bug, a crash, a bottleneck or a limit such as the frame rate. It passes its differential test and is hooked in. Progress is tracked in `symbols/functions.csv` |

R13 started with a measurement (#654): it named what bounds a frame in the heaviest areas, and the other issues started only where the numbers supported them. It cut the GPU thread's work with copy threads for CPU-modified pages (#777, #790), reuse of the pipeline lookup (#780, #824) and staging of only the copied bytes in the copy shader HLE (#832, #834). Two ideas were measured and dropped: uploading user data only when it changed (#776, below the spread between runs) and a larger write-fault window (#778, the guest threads never bound a frame). Splitting the GPU thread into two stages with one owner per cache was assessed and not built (#779: after #780 the GPU itself bounds the heaviest segments). R13 ran alongside R7, which kept priority.

R10 and R11 ran alongside R7, which kept priority: a fault that blocks play comes before any option (maintainer decision, 2026-11-19). Both started with research issues that found the engine's own parameters (vblank rate, frame-rate assumptions, post-processing, camera, shadows, draw distance); the options built on what they found. Each option keeps the game's own behaviour as the default, so 30 FPS and 1x stay as they are on `main`.

R8 ran in four parts, each a GitHub milestone: R8.1 (shader and pipeline cache), R8.2 (frame times), R8.3 (frame rate) and R8.4 (resolution). Render hooks get issues only when a measurement finds a bottleneck that the renderer can't fix.

The launcher of R9 is our own SDL3 and Dear ImGui window and starts `bb-run` as a separate process, with the `bb-run` options of R8.4 (#510). Its layout and behaviour are in `docs/design/launcher.md`. Installing from PKG files would need Sony's PKG and PFS keys, which `docs/source-policy.md` rules out, so the player brings the game as a folder (maintainer decision, 2026-11-16).

`src/gpu` was removed in R4 (#435), with the memory mirror, `bb-gcn` and the options of the old path (maintainer decision, 2026-11-09).

Each completed milestone ends with a minor release after its review (`docs/releasing.md`). Some milestones up to R11 have a tag of their own (`m1` to `r11`, see `git tag`) and none has a release.

## The reference run

The official shadPS4 v.0.21.0 Linux release on the reference machine (RADV on an RX 7800 XT), 2026-11-09, without firmware files, with a read-only game directory and input from a virtual gamepad:

- It went through the main menu, character creation with the name dialog, the intro and clinic gameplay, then saved and continued.
- It ran 25 minutes without a crash and without an amdgpu fault in the kernel log.
- It held 30 FPS, the game's cap, in the menu and the clinic, with 5.6 GB peak memory.

The test rig (`docs/test-rig.md`) repeats the run from a fresh data directory to first control in the clinic, and measures the reference and `bb-run` side by side in the clinic, in Central Yharnam with and without combat, and in sound.
