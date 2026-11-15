# Frame rate (R8.3)

How the game paces its frames and steps its simulation, and what has to change to run it at 60 FPS (#509). #514 builds the `bb-run` option on it. Our own analysis of the pinned eboot; third-party frame-rate patches are off-limits (`docs/source-policy.md`). Addresses are eboot link addresses (the guest runs them at 0x10000000 + address). Names and types are in `symbols/functions.csv` and `symbols/types/frame.h`.

## Result

The 30 FPS cap is two separate things in the game's main loop, and both have to change:

1. **The wait.** The frame pacer `flipper_wait_frame` (0x2034770) spins on `gettimeofday` until `SprjFlipper::target_frame_time` (+0x18) has passed since the previous frame. The value comes from the flip mode, default 4 (`30FPS_withoutSkipTearing`): 1/30 s. The game has a 60 FPS mode of its own, flip mode 2 (`60FPS_withoutSkip`, target 1/60 s), selected only by a debug configuration key.
2. **The step.** The main loop tick `main_loop_tick` (0x2018d20) hands every task a fixed time step of 1/30 s (an immediate at 0x2018e36), whatever the frame really took. The simulation advances 1/30 s per frame. At 60 FPS with that step the game runs twice as fast; with the step at 1/60 s it runs at normal speed.

The video side needs nothing: the game sets `sceVideoOutSetFlipRate(handle, 0)` and flips through `sceGnmSubmitAndFlipCommandBuffers`, and our video out takes a flip at every vblank.

## The main loop

`game_main` (0x1e4a6c0) runs `while (main_loop_tick()) {}`, then up to 100 iterations of `shutdown_loop_tick` (0x2018f00, the same task tick with no pacing, 333 µs apart). One `main_loop_tick`:

1. `flipper_wait_frame(SprjFlipper)` (0x2018e22). The `SprjFlipper` singleton (pointer at 0x55404f8, 0x2c8 bytes) is created on the first tick by `flipper_construct` (0x2034520).
2. Builds an `FD4TaskData` on the stack, {vtable 0x52efd40, `delta_time` = 0x3d088889 = 1/30} (0x2018e27–0x2018e36), and passes it to `task_manager_run_frame` (0x20512a0, `FD4::FD4TaskManager` singleton at 0x54b2e30). That runs the task lines (task group names at 0x49c0076–0x49c0bb6: `FrameBegin`, `SystemStep`, `PadStep`, ..., `HavokWorldUpdate_Pre`/`_Post`, `CameraStep`, `SoundStep`, `GraphicsStep`, `DrawStep`, `Flip`, `FrameEnd`) with that task data, and `set_global_task_data` (0x103f9f0) copies its `delta_time` into the global `FD4TaskData` at 0x54b7e00 (read through `global_task_data`, 0x103f9e0).
3. Returns `SprjWindow`'s virtual +0x18 (called at the start of the tick) and not a system object's virtual +0x98 (0x2018ed4): whether the game goes on.

## The wait: `flipper_wait_frame` (0x2034770)

Per call:

1. **Mode.** `heavy_last` (+0x275) = `heavy` (+0x276), `heavy` cleared. The mode is `flip_mode_heavy` (+0xc) when `heavy` was set, else `flip_mode` (+8). The switch at 0x203480d sets:

   | Mode | Name (table 0x535a920) | `present_interval` +0x10 | `tearing` +0x14 | `target_frame_time` +0x18 | `foreground_skip_max` +0x268 | `background_skip_max` +0x26c |
   |---|---|---|---|---|---|---|
   | 0 | `30FPS` | 2 | 1 | 1/30 | 1 | 30 |
   | 1 | `30FPS_withoutSkip` | 2 | 1 | 1/30 | 0 | 30 |
   | 2 | `60FPS_withoutSkip` | 1 | 1 | 1/60 (0x3c888889, 0x2034840) | 0 | 30 |
   | 3 | `30FPS_withoutTearing` | 1 | 0 | 1/30 | 1 | 30 |
   | 4 | `30FPS_withoutSkipTearing` | 1 | 0 | 1/30 (0x3d088889, 0x203487e) | 0 | 30 |

   `flipper_construct` reads both modes with `config_get_int(config, L"Game.FlipMode", fallback)` (0x20eba00; fallback 4 at 0x2034574 for `flip_mode`, 3 at 0x20345c2 for `flip_mode_heavy`), clamped to 4. The key is a debug setting; with no configuration the fallbacks hold, so the game runs mode 4 and, while `heavy` is set, mode 3. `heavy` is set every frame by 0x193ac10 while the session (`SprjSessionManager`, 0x5540290) lists more than one player (0x193ac78–0x193ac8a), so online play with other players runs `30FPS_withoutTearing`. Overrides after the switch: `force_default_mode` (+0x2c4, set by 0x15b9e30) gives mode 4's values and skip 0; the debug skip maxima (+0x2bc, +0x2c0) replace the skip maxima when not negative; `clear_skip_once` (+0x271, set by 0x1cfa670) and `no_skip` (+0x272, set on creation, cleared by 0x1d97390 in the graphics step) zero them.
2. **Wait.** `prev_time_us` (+0x20) = `now_time_us` (+0x28); now = `gettimeofday` (µs). Target T = `target_frame_time` in µs. Unless `force_default_mode` or `skip_wait` (+0x273) is set, it loops until `now - prev_time_us >= T`, reading `gettimeofday` (libScePosix, 0x2034af0–0x2034b6f) each time. `window_is_foreground` (0xfe3980) always returns 1, so the foreground branch runs: a pure busy wait (48 % of all CPU samples of the process in the R8.2 perf run, `docs/test-rig.md`). The background branch would `sceKernelUsleep(remaining - 5 ms)` when more than 5 ms remain. The wait is measured from the end of the previous wait, so the frame interval is max(T, the frame's work).
3. **Frame skip.** With a skip maximum N > 0 the wait is shortened after late frames: the durations of the last n ≤ N frames, as long as they were late, are summed from the 32-entry ring `records` (+0x60), and while the previous frame was late the wait becomes `(n+1)·T − sum`, at least T/3. With N = 0 (mode 4, mode 2) there is no catch-up: a late frame is not made up, the game slows down.
4. **Record.** The frame's duration goes into `records[++record_index & 31]` with the late flag; `delta_time` (+0x264) = duration in seconds; `late` (+0x270) = duration > 1.01·T (with skip, against the summed budget). A late frame sets `present_interval` to 0 for this frame. `recent_delta[16]` (+0x278) and `fps` (+0x2b8, 16 / sum) feed the debug menu (`flipper_register_debug_menu`, 0x2035630; labels `flipper_debug_label`, 0x2035480).

The measured `delta_time` (+0x264) is not read by game code: none of the 21 functions that load the singleton reads it, and the vtable (0x535a910) has only destructors. The simulation never sees the real frame time.

## The present

`graphics_apply_flip_settings` (0x1d978e0, per frame unless `skip_wait`) copies `present_interval` to the graphics object (0x55406c8, +0x100) and `tearing` to +0x104 and 0x513ac84. The render thread presents through `swap_chain_present` (0x21b2fb0, `GXSwapChainCtrl`, `presentIntervalType` 0 to 4): when its own timing flag is set it waits until `present_interval` 60 Hz periods since its last present (busy wait on `gettimeofday`, with `sceKernelUsleep` for more than 17 ms) and passes 0 or 1 on. `present_flip` (0x26d5dd0) and `submit_and_flip` (0x26d6e50) submit with `sceGnmSubmitAndFlipCommandBuffers`, flip mode 1 (vsync) for a non-zero interval and 2 (hsync) for 0; when the flip queue is full, `submit_and_flip` polls `sceVideoOutGetFlipStatus` and waits on the flip event queue (`sceKernelWaitEqueue`, 0x26d6f26). Video out is set up at 0x26d5e90 with `sceVideoOutSetFlipRate(handle, 0)` (0x26d61ee) and `sceVideoOutAddFlipEvent` (0x26d6234). With interval 1 (modes 2 to 4) the present waits at most one 60 Hz period, so it does not cap a 60 FPS frame; modes 0 and 1 (interval 2) would cap at 30 a second there too.

Not part of the pacing: `sceKernelGetProcessTime`, `sceKernelReadTsc`, `clock_gettime` and the RTC ticks (`docs/formats/kernel-imports.md`, `docs/formats/rtc.md`) have no call site in the loop above.

## The step

The step is a time in seconds that every update gets from the task data. Most of the game reads it from there; the tick's 1/30 reaches the task tree, and update functions pass it on as a float argument (for example the character update `chr_frame_update`, 0x1548740, which builds task data from its `dt` argument for its sub-updates). A late frame does not make the step longer, so the game slows down under load instead of skipping.

Some code does not take the step from the task data but has 1/30 built in. These sites do not follow a change of the tick's step and would run at twice the speed at 60 FPS. Found by a scan for the float 1/30 (0x3d088889, 107 dwords in the text segment, immediates and per-unit read-only copies) and a sample of the uses of 30.0f; the list is not exhaustive.

**Per-frame steps with a literal 1/30, each a candidate for #514 or its own issue:**

| Site | Function | What | At 60 FPS |
|---|---|---|---|
| 0x17f9b71 (`mov [rbp-0x48], 1/30`) | `havok_world_step` 0x17f99d0 | The Havok physics world step: builds the step record {1/30, `[this+0xc8]`, a step counter} and runs the collide (0xb5a2e0) and solve (0xb5b100) stages once. A member functor of the Havok manager (built at 0x17f9272), most likely the `HavokWorldUpdate_Pre` task | physics steps 1/30 s per frame: twice as fast |
| 0x63fc3c | `hkp_world_cinfo_init` 0x63fad0 | World creation defaults, 1/30 at +0x70 (a Havok world parameter, `expectedMinPsiDeltaTime` by its place among the defaults); the same value sits in three data blocks at 0x2bda86c, 0x2bdc3d4 and 0x2bdf7e4 | a tuning value for the step; set it to the new step with the step |
| 0x1572a7d | `chr_pad_step` 0x1572900 | A character's pad (`FD4PadManager`) stepped with its own task data of 1/30, called from 0x1548740 and 0x193ac10, which have the step at hand | pad timers (holds, repeats, input buffer) twice as fast |
| 0x1e2bcd4 | `chr_ai_step` 0x1e2bc10 | Per-character navigation and Havok AI (`SprjWorldNvmManager`, `SprjHkAiManager`) with task data of 1/30 | AI navigation steps twice as fast |
| 0x12a4416 (`addss [+0x2c], 1/30`) | `target_bank_scan_tick` 0x12a43b0 | `SprjTargetBankManager`: a timer advanced by 1/30 per call rescans all characters when it reaches an interval; a 100-entry ring indexed at 30 slots a second (`seconds * 30`, 0x12ab623) | rescans twice as often, and the ring covers half the time it is read as covering |
| 0x180f647 | `chr_list_fade_step` 0x180f5e0 | a fade `+0x1c0 = min(fade + 1/30, 1)` per call over the character list | fades in 0.5 s instead of 1 s |
| 0x143e2b2 | 0x143ac60 (`RendMan`) | smoothing `x += (target - x) / 30` per call | converges twice as fast (minor) |
| 0x173456e, 0x15dc1f7, 0x1044943, 0x1f77d4a, 0x227784b, 0x18bc4f7 | menu item lists (`Ins_change_tab_select`), the equipment icon dialog, and four updates not identified | an `update(1/30)` call with a literal | menu and UI animations twice as fast |

**Task data with a literal 1/30 that probably needs no change** (not verified that the callees ignore `delta_time`): the event system runs a new event once at creation (`SprjEmkSystem`, 0x12efd00); a menu task (0x1b96b00); teardown (0xf73330); and about 30 small step-machine functions (0x1aa3fc0–0x1aa6080, 0x1ab5390–0x1ac3ef0, 0x1b03210–0x1c12bf0) that enter a state through a virtual call with task data of 1/30, most referencing `FrpgNetMan` (the online flow).

**Leave unchanged:** the many `(float)frames * 1/30` conversions (for example 0x17b55ed, 0x17c8754 in the event system, 0xf1e607 and its neighbours, 0x1dbc308 in the AI): they turn frame numbers from the game's data, authored at 30 frames a second, into seconds, and stay right at any frame rate. The 30.0f constants sampled are mostly limits of debug sliders, angles and network values.

**Not resolved:** the Havok behaviour (animation) update and where its time step comes from (`physicsDeltaTime` 0x479e0ec, `hkbGetTimestep` 0x47d5e84); the functor bound to `HavokWorldUpdate_Post` (0x17f9ce0 is the likely one and passes 1/30 in xmm0 from 0x4927554 to 0xfde7f0, whether it is read is unknown); what the menus, effects (`SfxMan`) and the cloth update take as their step.

## Check run

Without tracked code: a local `bb-run` (`relwithdebinfo`, not committed) wrote bytes into the eboot after loading, and `rig.py` measured the presents (RADV, `--window`, a shared `--cache-home`, 2026-11-15). Variants:

- **base**: unchanged.
- **60**: flip mode fallback 4 → 2 (byte 0x2034575 = 0x02): target 1/60 s, step still 1/30 s.
- **60+step**: also the tick's step 1/30 → 1/60 (0x2018e39 = `89 88 88 3c`).

`main-menu` (runs `20261115-122213`, `-122259`, `-122333`, suffix `-bb-run-main-menu`). `logos` is from the splash hide to the menu standing still for 6 s; its length includes the loading and the 6 s.

| Variant | `logos` presents, fps, p50 | `menu` fps, p50, p99 | `logos` length |
|---|---|---|---|
| base | 682, 29.97, 33.33 ms | 30.0, 33.33, 33.39 ms | 22.8 s |
| 60 | 914, 59.65, 16.67 ms | 60.0, 16.67, 16.71 ms | 15.3 s |
| 60+step | 1283, 59.76, 16.67 ms | 60.0, 16.67, 16.72 ms | 21.5 s |

The clinic from a save (local scenario: Continue, stand 15 s, two 2 s walks, camera, 8 dodges, 8 attacks; runs `20261115-122450`, `-122742`, `-123027`, suffix `-bb-run-fr509`), per segment presents a second and p50 / p99 / max interval:

| Segment | base | 60 | 60+step |
|---|---|---|---|
| `play-stand` | 30.0, 33.33 / 33.40 / 33.55 ms | 59.83, 16.67 / 18.72 / 20.32 ms | 59.65, 16.67 / 18.76 / 20.61 ms |
| `play-walk` | 30.0, 33.33 / 50.01 / 50.02 ms | 59.73, 16.67 / 18.75 / 19.51 ms | 58.37, 16.68 / 19.47 / 33.34 ms |
| `play-camera` | 30.04, 33.33 / 36.42 / 50.05 ms | 59.73, 16.67 / 18.80 / 20.70 ms | 59.71, 16.67 / 18.95 / 19.70 ms |
| `play-dodge` | 30.0, 33.33 / 33.39 / 33.64 ms | 60.0, 16.67 / 16.72 / 16.91 ms | 60.0, 16.67 / 16.74 / 17.00 ms |
| `play-attack` | 30.0, 33.33 / 33.39 / 35.38 ms | 60.0, 16.67 / 16.72 / 17.01 ms | 60.0, 16.67 / 16.73 / 19.05 ms |

All six runs finished their scenario with no GPU fault in the kernel log. What they show:

- **The wait is the cap.** The fallback flip mode alone takes the game from 30 to 60 presents a second, in the menus and in play; nothing else in the game or in our video out holds it at 30. The clinic holds 60 on this machine, with a p99 of about 18.8 ms: now and then a frame misses its 16.7 ms slot by about 2 ms.
- **The step is per frame.** At 60 FPS with the 1/30 step the timed part of the logo sequence (its length minus the 6 s stable wait) shrinks from 16.6 s to 9.2 s, and in the clinic runs (the same segment, 1 s later start) from 14.5 s to 7.2 s: the game runs at twice its speed. With the step at 1/60 it is back to 15.4 s and 14.3 s. After the first 2 s walk the hunter of the 60 run stands nearer the wall (picture `02-walked`) than in base and 60+step, which end in about the same place.
- **The literal sites can be changed in place.** A fourth clinic run (`20261115-123635`) also set the Havok step (0x17f9b74), the pad step (0x1572a80) and the AI step (0x1e2bcd7) to 1/60. It finished with no GPU fault, the logo sequence took its normal 14.4 s, and dodges and attacks ran at 60.0 presents a second; the other segments ran at 44 to 54, because another program used the GPU of the shared machine during that run, so it gives no frame times. Whether play is right with these changes needs input-driven checks (#514).

## What #514 changes

A target frame rate F (30 default, 60) and step 1/F. At F = 30 every change below must give the game's own values, so the default run is unchanged.

1. **Wait: replace `flipper_wait_frame` (0x2034770, 0x84c bytes by its FDE).** Same logic, with `target_frame_time` = 1/F after the mode switch for modes 3 and 4 (the default and the online one). Keep the mode's `present_interval` (1) and `tearing` (0): mode 2 would also set `tearing` = 1, which we have not followed through the render thread, so switching the fallback to 2 (what the check run did) is not the way. The busy wait may become a sleep for all but the last millisecond (the background branch's `sceKernelUsleep` shows the game accepts one); that saves the core that `game main` spins on, and is optional.
2. **Step: replace `main_loop_tick` (0x2018d20, 0x1df bytes).** The same calls, with `delta_time` = 1/F in the task data it builds (0x2018e36).
3. **Physics: replace `havok_world_step` (0x17f99d0)** with the step record's 1/F, and give the world parameter of `hkp_world_cinfo_init` (0x63fad0, +0x70) and the three data blocks (0x2bda86c, 0x2bdc3d4, 0x2bdf7e4, if they are used) the same 1/F.
4. **The literal per-frame sites of "The step"**: `chr_pad_step` (0x1572900), `chr_ai_step` (0x1e2bc10), `target_bank_scan_tick` (0x12a43b0, also the ring's 30 slots a second), `chr_list_fade_step` (0x180f5e0), the `RendMan` smoothing (0x143ac60) and the menu `update(1/30)` calls. Each is a decomp candidate of its own, checked at 60 FPS against a 30 FPS run (the same input, the same outcome).

Items 1 and 2 are what #514 needs for a 60 FPS option that runs at the right speed in menus and the world (the check run). Items 3 and 4 decide whether play is right. Expected to break or change at 60 FPS until they are done, and to be checked in combat and outdoor scenes (R8.3 needs the areas R7 opens): physics (ragdolls, thrown objects, breakable props, cloth, which may run its own step), the player's input timing (pad buffer, charge attacks with `R2`, dodges), AI navigation and the enemy target scan, hit-stop (`SprjChrHitStopModule`, not analysed), fades and menu animations. Animation and events that convert their 30 FPS frame data to seconds should stay right.

Frame times at 60: the flip grid of #528 (`src/hle/video/vblank_grid.hpp`) was tuned on the 30 FPS clock; at 60 FPS each frame has one vblank, and the clinic showed a p99 of 18.8 ms where a frame misses its slot by a little. #514 should run `rig.py perf` at both rates.

## Open questions

- Mode 2's `tearing` = 1: where `graphics +0x104`, 0x513ac84 and the render list's byte at +8 end up (presumably the hsync flip when late).
- Whether the Havok behaviour and cloth updates take the task data's step; the `HavokWorldUpdate_Post` functor's 1/30 (0x17f9ce0).
- Whether the step-machine functions with task data of 1/30 read `delta_time`.
- `skip_wait` (+0x273, set by 0x1cf4c70 with `SprjScaleform`) and `request_0x274` (sets +0x4c, +0x4d and 60 at +0x50 of the object at 0x5540e00): what state skips the wait.
- The remaining uses of 1/30 and 30.0f that the survey did not classify.
