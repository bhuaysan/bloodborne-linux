# R6: fitting the derived code to this port

R3 to R5 imported shadPS4 code nearly unchanged (`src/shadps4/`) behind a host layer (`src/shadps4_host/`). R6 removes the emulator structure a single-game port doesn't need. This note fixes the rules for that work and lists what stays in shadPS4's shape on purpose.

## The import becomes our code

From R6 on, files under `src/shadps4/` are edited in place, like any of our files. There is no fork kept in sync with upstream: we follow v.0.21.0 and take later upstream fixes by hand when an issue needs them.

- A file that changes keeps its copyright, SPDX and `// Source:` lines. Its `// Changed for bloodborne-linux, <date>` line gets the date of the latest change (the notice of changes that `docs/source-policy.md` asks for).
- `tools/import-shadps4/import_shadps4.py` stays as the record of how the import was made. `--check` reproduces the tree of the import commits, not the current one, and no test runs it.
- `third_party/` stays unchanged copies.
- A host stub in `src/shadps4_host/` whose last caller is gone is deleted in the same change.

## How each rewrite is checked

Every R6 change is a refactor: the game must behave as before. Each PR shows:

1. `tools/run-tests.sh` with the game, including the shader corpus test (`tests/shadps4/corpus_test.cpp`: the same nine known failures, no new one) and `video_core_test`.
2. A `rig.py compare` of a `bb-run` run against the reference for `main-menu` (frame difference at the noise floor) and the `new-game-name` scenario (reaches the intro, no GPU fault in the kernel log), with `bb-run` built in the PR's worktree.
3. Frame times (median and p99 frame interval) of the `main-menu` run, next to the numbers of a `bb-run` run from `main` made the same day. The game caps at 30 FPS, so this catches stalls, not small speedups.

## What goes

| Area | Change |
|---|---|
| Settings | `EmulatorSettings` (`FixedSettings`, all constants) is gone (#487). Dead branches are deleted. Vulkan validation (`--validation`, `--validation-sync`, `--validation-gpu`), the present mode (`--present-mode`) and the device (`--device`) are `bb-run` options in `src/tools/run_options.*`, handed to the renderer as `Vulkan::RendererOptions` (`vk_options.h`). Pad, IME, audio and font constants sit in the code that uses them. What stays: FSR and RCAS as constants in `vk_presenter.cpp` |
| Pipeline cache | No longer off (#507): `cache_storage.cpp` is built with the plain directory store, the zip archive path (`pipeline_cache_archived`, miniz) and the host stubs are deleted, and the cache is on by default under the XDG cache directory (`bb-run --no-pipeline-cache` turns it off). See `docs/design/renderer.md` |
| Debug state and tools | `DebugState` register dumps, devtools layers, the friends, invitation and notification layers, Tracy, RenderDoc, the presenter's screenshot path, host and guest GPU markers. Draw and dispatch counters move to a plain stats struct that `bb-run` logs |
| Shader debugging | Dumping, hash-keyed shader patches and shader collection |
| Other platforms and consoles | `_WIN32` and `__APPLE__` branches, MoltenVK, PS4 Pro (`sceKernelIsNeoMode`) paths. Readbacks, `userfaultfd` tracking, null GPU, direct memory access and copied GPU buffers, which are all off |
| Singletons | `Core::Memory` (the `MemoryManager` singleton) is passed by reference to `Liverpool`, the rasterizer, the caches and `PageManager` |
| Presenter | The frame goes to the swapchain with a direct letterboxed blit instead of an ImGui image. ImGui draws only while the IME dialog is open |

## The present path

`Presenter::Present` clears the swapchain image to black and blits the frame image into the letterboxed rectangle (`FitImage`). The post-processing pass stays in `PrepareFrame`, in front of the blit. It is not only a gamma step: it is the one place that converts the game's buffer format to the frame image (the A2R10G10B10 formats have no sRGB variant in Vulkan, so an sRGB-encoded 10-bit buffer is decoded there) and that writes the scaled FSR output into the window-sized frame image. A blit cannot do either, and R8's resolution work needs the pass. The blit only moves the finished frame to the swapchain. ImGui is recorded on top (load, not clear) only while a layer, the IME dialog, is open.

## What stays in shadPS4's shape, and why

| Kept | Reason |
|---|---|
| Driver quirks in `Profile` and `vk_instance` (NVIDIA, Intel, KosmicKrisp, AMD proprietary) | They are keyed by driver ID, cost nothing on RADV and keep the port running on other Linux drivers |
| FSR and the post-processing pass | Presenter scaling for R8's resolution work. The post-processing pass stays in the present path (see below) |
| ImGui core and its Vulkan backend | The IME dialog (the name entry in character creation) is an ImGui UI of about 5,500 lines. Rewriting it gains nothing for play |
| `Core::Signals`, `Input::GameControllers`, `Common::Decoder`, `IrqC` singletons | One instance per process by nature, few readers, and our runtime owns their lifetime already |
| SDL3 for the window, gamepads and audio | One backend, and the rig's virtual gamepad and audio capture build on it |
| Logging classes | The macros need them. Unused classes cost nothing |
