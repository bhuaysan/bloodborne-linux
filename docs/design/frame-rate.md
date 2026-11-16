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
| 0x17f9b71 (`mov [rbp-0x48], 1/30`) | `havok_world_step` 0x17f99d0 | The Havok physics world step: builds the step record {1/30, `[this+0xc8]`, the thread pool's thread count + 1, 0} and runs the collide (0xb5a2e0) and solve (0xb5b100) stages once. A member functor of the Havok manager (built at 0x17f9272), most likely the `HavokWorldUpdate_Pre` task | physics steps 1/30 s per frame: twice as fast (replaced in #553) |
| 0x63fc3c | `hkp_world_cinfo_init` 0x63fad0 | World creation defaults, 1/30 at +0x70 (a Havok world parameter, `expectedMinPsiDeltaTime` by its place among the defaults); the same value sits in three data blocks at 0x2bda86c, 0x2bdc3d4 and 0x2bdf7e4 | none found: not called from boot to play in Central Yharnam, and not the stepped world (#553) |
| 0x1572a7d | `chr_pad_step` 0x1572900 | A character's pad (`FD4PadManager`) stepped with its own task data of 1/30, called from 0x1548740 and 0x193ac10, which have the step at hand | pad timers (holds, repeats, input buffer) twice as fast |
| 0x1e2bcd4 | `chr_ai_step` 0x1e2bc10 | The Havok AI world's step (`SprjWorldNvmManager`, `SprjHkAiManager`) with task data of 1/30 | the Havok AI world steps 1/30 s per frame (replaced in #554) |
| 0x12a4416 (`addss [+0x2c], 1/30`) | `target_bank_scan_tick` 0x12a43b0 | `SprjTargetBankManager`: a timer advanced by 1/30 per call rescans all characters when it reaches an interval; a 100-entry ring indexed at 30 slots a second (`seconds * 30`, 0x12ab623) | rescans twice as often, and the ring covers half the time it is read as covering |
| 0x180f647 | `chr_list_fade_step` 0x180f5e0 | a fade `+0x1c0 = min(fade + 1/30, 1)` per call over the character list | fades in 0.5 s instead of 1 s |
| 0x143e2b2 | `chase_cam_update` 0x143ac60 | the player's chase camera eases its follow weight `+0x198 += (+0x19c - +0x198) / 30` per call (first read as a `RendMan` smoothing; see #558 below) | one of the camera's per-call smoothings |
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

## The option (#514)

`bb-run --frame-rate 30|60` (default 30) sets the target frame rate F; `rig.py run` and `rig.py perf` pass it on (`--frame-rate`, to `bb-run` only). Two game functions are replaced, in `src/game/frame/`, and are hooked at every frame rate:

1. **`flipper_wait_frame` (0x2034770, 0x84c bytes by its FDE).** The same logic, with `bb::game::frame_step()` = 1/F as the frame time of modes 3 and 4 (the default and the online one) and of the forced default mode. Modes 0 and 1 keep 1/30 (their present interval of 2 caps them at 30 anyway) and mode 2 keeps its 1/60 and its `tearing` = 1, so the game's own 60 FPS mode is not what the option selects. The busy wait stays a busy wait: a sleep would change the frame times we measure against, and it costs only the core the game's main thread already has.
2. **`main_loop_tick` (0x2018d20, 0x1e0 bytes by its FDE).** The same calls, with `delta_time` = 1/F in the task data it hands to the task manager, so every task and the global task data (0x54b7e08) get the step.

At F = 30, 1/F is the game's own 0x3d088889, and the replacements do what the originals do: their differential tests (`tests/game/frame_test.cpp`) run the original and the replacement from the same state and compare every byte they touch. The pacer reads the clock, so the tests give `gettimeofday` a fake wall clock (`bb::hle::kernel::set_wall_clock`) that moves on by a fixed step with every read; the original and the replacement then wait the same number of reads. The singletons are fakes in the harness's arena whose virtual functions are ours, and the task manager's frame runs for real down to a fake task line that records the task data it gets. Not covered by the tests: the pacer's background branch (`window_is_foreground`, 0xfe3980, always returns true), the creation of the pacer on the first tick (it needs the game's configuration and allocator), the `fd4_singleton_missing` reports (0x20b55b0) and the system object's creation (0x20e2550); these are for the disassembly review.

Replacing the pacer moved `bb-run`'s main-loop probe: it counted the pacer's `gettimeofday` calls by their return address, which now lies in our code. The pacer reports each call to `bb::game::set_frame_observer`, and the probe counts 60 calls.

**Hooks for the other sites.** Function-entry hooks are the only mechanism the repository has for game code; the HLE data initializers cover imported data only. The 1/30 of the remaining sites is an immediate in code (`mov [rbp-0x48], 0x3d088889`) or a load from a constant next to the function. Changing those bytes in place would be a new mechanism, outside the differential harness and the disassembly review, so each site is replaced as a function, like the two above. Two sites need nothing:

- **`HavokWorldUpdate_Post` (0x17f9ce0).** It loads 1/30 into `xmm0` (0x17f9d26, from 0x4927554, read by nothing else) and tail-calls 0xfde7f0, which never reads `xmm0`: it zeroes it (0xfde83e) before the only call that could take a float. The 1/30 is dead.
- **The three data blocks with 1/30 at the place of `hkp_world_cinfo_init`'s +0x70** (0x2bda86c, 0x2bdc3d4, 0x2bdf7e4) have no references in the analysis.

### Check runs (#514)

`relwithdebinfo`, RADV, `--window`, a shared `--cache-home` for the clinic runs, 2026-11-15, runs in `analysis/rig/runs/` (stamps `20261115-...`). The machine was shared: no other program used the GPU, but another program's processes kept the CPU load average at 13 to 16 on 16 threads until about `-1630` and at 3 to 5 after. Runs in the busy part are marked; the 30 FPS runs show it too (a p99 of 50 to 52 ms where the quiet runs have 33.4 ms).

**Frame times at 60.** `clinic-fight` (the `rig.py perf` route), three runs at 60 that finished (`-162527`, `-163337`, `-164359`, the first two in the busy and the quiet part), against the five reference runs of 2026-11-14 (`rig.py perf --branch ... --reference ...`), p50 / p95 / p99 / max in ms, median over the runs:

| segment | 60 FPS | reference (30 FPS) |
|---|---|---|
| `play-stand` | 16.67 / 16.73 / 18.73 / 19.31 | 33.33 / 33.41 / 33.47 / 49.97 |
| `play-walk` | 16.67 / 18.69 / 18.77 / 19.86 | 33.33 / 33.44 / 50.06 / 50.09 |
| `play-camera` | 16.67 / 16.70 / 16.82 / 18.76 | 33.33 / 33.41 / 50.01 / 50.19 |
| `play-dodge` | 16.67 / 16.70 / 16.72 / 17.03 | 33.33 / 49.97 / 50.05 / 50.27 |
| `play-attack` | 16.67 / 16.70 / 16.73 / 16.95 | 33.33 / 33.39 / 33.43 / 50.11 |
| `play-lockon` | 16.67 / 16.70 / 16.72 / 17.50 | 33.33 / 33.39 / 33.63 / 50.12 |
| `play-walk-again` | 16.67 / 16.69 / 16.72 / 16.86 | 33.33 / 33.38 / 33.42 / 50.00 |

No stutter in play. The clinic holds an even 16.7 ms median; in `play-stand` and `play-walk` about 1 to 5 % of the frames miss their slot by 2 ms (18.7 ms), as in #509's check run, which the flip grid of #528, tuned on 30 FPS, may explain. A fourth run at 60 (`-161738`, busy part) ended in `play-dodge` with a segfault of a `GXWorker` thread at guest 0x263b8e7, reading 0x4300000000 while unlinking from a list; the same address faulted in a 30 FPS `main-menu` run of `main` on 2026-11-12 (`20261112-133325`), so it is older than this change, and a frame rate twice as high may only make it more likely. The main menu at 60 (`-163245`): 60.0 presents a second, p50 16.67 ms, p99 16.72 ms.

**Game speed.** The local scenario `fr514` (as #509's, with pictures at fixed delays after each input: 0.35 s and 1.85 s after a backstep, 0.5 s and 2 s after an attack, 1 s after each 2 s walk and after a 2 s camera turn), two runs at 30 (`-160755`, `-161249`) and two at 60 (`-161515`, `-164910`, both 57 to 60 presents a second in every segment). The time from the logos to the still title is 15.51 and 15.53 s at 30 and 15.34 and 14.32 s at 60; the main menu (`main-menu`) took 18.77 s at 30 and 18.86 s at 60. The rig looks at a picture at most once a second, so these agree within that resolution, not within a frame. In the pictures the backsteps and attacks of the 60 runs are in the same phase as at 30 (the same pose 0.35 s and 0.5 s after the press), where #509's 60 run without the step was twice as fast. The mean absolute difference of the same pictures (0 to 255):

| pictures | 30 against 30 | 60 against 60 | 30 against 60 |
|---|---|---|---|
| attacks (8) | 1.55 | 2.12 | 5.07 |
| backsteps (8) | 2.23 | 1.92 | 3.97 |
| walks and camera turn (4) | 2.71 | 3.15 | 6.16 |

The two rates differ by about twice the noise of either, and the 60 runs agree with each other: something still depends on the frame rate. In the pictures it is the place of the camera and of the hunter, not the phase of an animation: after the first 2 s walk the hunter of both 60 runs stands a little nearer the shelf and the camera follows a little closer, and later pictures keep that offset. "Camera and hunter at 60 FPS" below traces it (#558).

**30 FPS is unchanged.** `rig.py compare` of the branch's `main-menu` at 30 (`-163040`) against two runs of `main` (`-163121`, `-163204`, the same occasion): noise floor 0.0257, ours 0.0254 (ratio 0.99), time to the menu 18.77 s against 19.82 and 18.75 s (ratio 0.97). `clinic-fight` at 30 (`-162014` busy, `-163848` quiet): 30.0 presents a second, p50 33.33 ms in every segment. The 60 FPS `main-menu` run passes the same comparison (ratio 1.49, within twice the floor; time ratio 0.98).

## What is left at 60 FPS

Each is a decomp issue of its own, checked at 60 against a 30 FPS run:

- **Havok world step** (`havok_world_step`, 0x17f99d0): done in #553, see "The Havok world step" below. The ragdoll corpses still settle faster at 60 (#567).
- **AI step** (`chr_ai_step`, 0x1e2bc10): done in #554, see "The AI step" below.
- **Target bank** (`target_bank_scan_tick`, 0x12a43b0, and the ring's 30 slots a second at 0x12ab623): rescans twice as often, and the ring covers half the time.
- **Fades** (`chr_list_fade_step`, 0x180f5e0) and the menu `update(1/30)` calls (0x173456e, 0x15dc1f7, 0x1044943, 0x1f77d4a, 0x227784b, 0x18bc4f7): twice as fast.
- **Chase camera** (`chase_cam_update`, 0x143ac60, 0x4e6c bytes): done in #557, see "The chase camera at 60 FPS" below.
- **`chr_pad_step`** (0x1572900) is not the player's input: its 1/30 goes to 0x1d145b0, which turns a matrix by the pad's axes times π/2 and −π/3 rad/s, a debug free camera behind a mode check. Low priority.
- **The offset of camera and hunter** of the check runs above: mostly the rig's inputs and the chase camera, see the next section. A small difference in the hunter's own motion after a short side step is not traced yet.


## Camera and hunter at 60 FPS (#558)

The 30-vs-60 picture difference of #514's runs has two causes: the way the rig's scenario times its inputs, and the chase camera's per-call smoothing. None of the known 1/30 sites (#553 to #556) is involved. A small difference in the hunter's own motion remains.

### The inputs: seconds, not steps

The scenarios hold the stick for a time on the wall clock (`stick`, `sleep 0.5`, `neutral`). The game advances 1/F per frame and never makes up a late frame (modes 2 and 4 have no frame skip). So a hold covers fewer game steps whenever frames miss their slot. At 30 FPS that hardly happens. At 60 FPS on a busy machine it does. Presents during the holds of the local scenario `fr558` (as `fr514`, with a 0.5 s camera turn, a 0.6 s walk and 1.5 s sideways walks, pictures right after them), CPU load average 6 to 12 from other jobs (2026-11-15):

| | 0.5 s turn | 1.5 s side walk |
|---|---|---|
| 30 FPS (3 runs) | 14 to 15 of 15 | 44 to 46 of 45 |
| 60 FPS (4 runs) | 22 to 25 of 30 | 55 to 90 of 90 |

The 60 runs turn the camera less and differ from each other: mean picture difference after the turn is 0.4 between 30 runs, 5.7 to 7.7 between 60 runs and 10 to 11 between the rates. #514's two 60 runs had 95 % and 99.5 % of their steps in the walk segment, so part of their offset is this too.

A check at 60 against 30 therefore has to hold inputs for a number of steps: the rig's `frames N` step, with N doubled at 60. `frames` polls the frame count every 2 ms near the end of its count, so it ends within a frame of the Nth present (#562; the runs below used a local rig copy with the same poll). The pacer's records show the holds exact: 15/30, 18/36, 45/90 presents.

### The chase camera

With equal steps per input (scenario `fr558f`, five runs at 30 and five at 60), the right-stick camera turn agrees between the rates. For each picture and run, the table gives the difference to the nearest other run of the same rate and to the nearest run of the other rate:

| pictures | same rate | other rate |
|---|---|---|
| after a 0.5 s camera turn (3 pictures) | 0.74 to 1.03 | 1.40 to 2.03 |
| after a 0.6 s walk | 3.23 | 4.89 |
| during and after a 1.5 s side walk (4 pictures) | 4.26 to 6.03 | 8.96 to 15.68 |

Walking sideways separates the rates. Every 60 run is 10 to 13 away from every 30 run right at the end of the walk, and the 60 runs are 2 to 9 apart. In the pictures the hunter sits in the same place on screen, while the view at 60 has panned further in the walking direction. After an R3 reset, which puts the camera behind the hunter, the 60 hunter faces further to that side. The stick moves him relative to the camera, so a camera that swings more while he walks also bends his path.

The camera is `chase_cam_update` (0x143ac60), called by `cam_mode_update` (0x14368b0, at 0x1436c00 and 0x1436e15) with the step in `xmm0`. 1/60 arrives there at 60, so the step itself reaches it. Its stick turn, the timers and the wall slide (+0x294) scale with the step. Several filters work per call instead:

| Field | Value (set by `chase_cam_construct` 0x143a0e0) | Use | Per call |
|---|---|---|---|
| +0x198 | 0.1 (imm 0x143a8b7), lerps toward +0x19c (0.1, 0x143a8c1) by 1/30 a call (0x143e2b2, rodata 0x4925e94), reset to +0x1a0 (0.25, 0x143a8cb) | follow point `+0x90 += (target − +0x90) · +0x198` (0x143ba97). The yaw is re-derived from the follow point every call. | yes |
| +0x1ac, +0x1b0, +0x1b4, +0x1b8, +0x1c4 to +0x1cc | 0.1, 0.1, 0.25, 0.03, 0.4, 0.4, 0.3 | lag of the camera position +0xf0 (0x143e2f2 to 0x143e980) | yes |
| +0x240 | 0.3 (0x143aa5b) | yaw and pitch easing while turning on its own (R3 reset, lock-on; 0x143c30f, 0x143c784, 0x143cea0) | yes |
| +0x1a8 | 0.3 (0x143a8df) | pitch pull toward +0x150 (0x143cef0) | yes |
| +0x330 | 0.05 (0x143a7de) | eases the camera parameters (FOV +0x50, +0x160, +0x188, +0x1f0, +0x248) toward the parameter row (0x143ae75 to 0x143af70); debug label "lock camera param interpolation rate" | yes |
| +0x184 | 0.1 (rodata 0x4925e68, one use) | `+0x184 += (+0x188 − +0x184) · 0.1` (0x143af8c), but 0x143bc70 sets +0x184 = +0x188 before its only read (0x143d545): no effect | no |

The follow filter is the one that explains the swing: the follow point catches up with the hunter faster at 60, so the camera turns further during a short walk. A per-call factor k matches 30 FPS at 60 as 1 − (1 − k)^½. For a decomp it becomes 1 − (1 − k)^(30·dt).

**Check runs.** Scenario `fr558s`: side steps of 10, 30 and 60 steps at 30 (doubled at 60), each followed by 45 steps of rest. Two runs per rate (`194619`, `194948` at 30; `194801`, `195131` at 60; stamps `20261115-…`, UTC). The first side step gives the clearest picture: 0.4 between the 30 runs right after it and 0.1 after the rest, 7.4 to 8.0 and 7.8 to 10.2 between the rates. The patched 60 runs below were measured against the nearer of the two 30 runs. The rows marked `fr558f` use that scenario's side walk and its two pictures instead (5 runs at 30):

| Variant at 60 (bytes written after loading) | end of step | after rest |
|---|---|---|
| none | 7.4, 7.9 | 7.8, 10.2 |
| Havok world step, AI step or both at 1/60 (0x17f9b74, 0x1e2bcd7; `fr558f`, 1.5 s side walk, 4 runs) | 10.3 to 12.1 | 7.4 to 9.6 |
| rodata 0x4925e94, 0x4925e68 and +0x330 at the per-60 value (`fr558f`, 1.5 s side walk) | 10.3 | 7.4 |
| follow filter: +0x198, +0x19c, +0x1a0 and 0x4925e94 (`204639`, `205003`) | 3.9, 6.1 | 9.1, 7.7 |
| follow filter, lag group and +0x240, +0x1a8 (`204821`) | 3.9 | 7.5 |
| `chase_cam_update` only every second call, with twice the step (`205343`, `205526`) | 0.8, 5.9 | 8.9, 9.0 |

Running the whole camera at 30 updates a second (the last row, a local call-site redirect in `bb-run`) brings the end of the side step to the 30 noise in one of two runs. The other run lands on the second of two outcomes that ±1 step of input timing gives at either rate (two 30 runs of `fr558f` differ by 7.7 the same way). So the camera's per-call filters are the main site. Scaling only the constants found does less (3.9 to 6.1): some per-call smoothing in the function is still missed, so a decomp has to go through all of 0x143ac60.

After the rest, the patched runs still differ by 7.5 to 9. The pictures show why: at the end of the step the camera matches and the hunter's pose differs slightly, and the rest of the camera motion follows his slightly different stop. That remaining difference is in the hunter's motion, not the camera, and it is not traced. Ruled out by reading the code or by runs:

- The character's turn toward the stick (`chr_turn_update` 0x150e690: the step per call is capped at rate · π/180 · dt) and the walk start ramp (`walk_anim_update` 0x1533940: `+= dt / ramp time`) scale with the step.
- `chr_anim_step_sync` (0x15985a0) pulls the character's animation step toward a reference phase with a per-call gain of 0.1 (rodata 0x4926b88, used at 0x1598663; its clamps scale with dt). Switching it off (`jne` at 0x15985b8 → `jmp`) changes nothing at 30 (0.2 to 0.4 against the unpatched 30 runs), so the phase error is zero in a walk. Setting the gain to 1 − 0.9^½ at 60 changes nothing either.
- The Havok world step (#553) and the AI step (#554): no change (table). The target bank (#555) and the fades and menu updates (#556) are not in the player's path.

### The hunter's side step

Measured with the camera wrapper of #557 in place (`main` at 8aa5863), scenario `fr558s`. Picture difference to the nearest 30 run (`233023`, `233355`, `234048`, `000552`, `000735`; the 30 run `233721` has 690×372 pictures and is left out; stamps `20261115-…`/`20261116-…`, UTC). Most 30 runs agree to 0.3; one (`233023`) lands on the other outcome.

| Picture | 30 vs 30 | 60 (`233209`, `233538`, `233905`, `234231`) |
|---|---|---|
| end of the 10-step walk | 0.01 to 5.2 | 3.4 to 5.3 |
| 45 steps later | 0.05 to 8.6 | 7.5 to 9.1 |
| 45 steps after the 30-step walk | 0.1 to 7.0 | 9.0 to 9.9 |

**Position, not pictures.** A local `bb-run` logs the camera state every call, and through the `chr` argument of `chase_cam_update` the hunter's object: `[chr+0x58]+0x350` is his world position (x, y, z), updated every frame; +0x320 to +0x348 hold his facing. Runs at 60 give the same position track to the last digit, so one run per variant shows a change. The first side step from standing (10 steps at 30, `g30`/`e30` against `g60`/`e60`, x by game time from his first move):

- Up to 200 ms the two rates agree to 0.005 m. He turns to the right in the first frame and speeds up to about 4 m/s.
- Then comes one frame without motion, after which he walks at about 0.6 m/s at both rates.
- At 30 that frame comes 33 ms earlier than at 60. At 60 he gets two more fast frames and stops 0.13 m further to the right (0.28 m in the 30-step walk). The two outcomes of the 30 runs differ by the same amount: when the fast phase ends depends on input timing, and at 60 it always ends late.
- The later steps keep that offset; the back step of the 10-step walk is as long at both rates. The 60-step walk runs into a shelf, which puts the hunter at the same place at both rates.

So the remaining 30-vs-60 difference is when the step's fast phase ends, not a speed or a filter. Ruled out at 60 by runs (positions or pictures; stamps `20261116-…`):

| Variant at 60 | Runs | Result |
|---|---|---|
| `chr_frame_update` (0x1548740) every second call per character with twice the step | `234610`, `234935`, `235300` | pictures as unpatched |
| the animation update over all characters (0x15af4f0, and the per-entry 0x15b2910) the same way | `000043`, `000226`, `000409` | pictures as unpatched |
| both together | `004621` | the fast phase ends 1/60 s earlier, still 1/60 s later than at 30 |
| Havok world step at 1/60 (0x17f9b74, #553) | `001240`, `001422`, `001604` | pictures as unpatched |
| AI step at 1/60 (0x1e2bcd7, #554) | `004825` | position track as unpatched |

`hkbGetTimestep` (string 0x47d5e84) is a Havok Script function registered at 0x976a4d; its body (0x9779d0) returns a float of the script's context (+0x30). It reads a step, it doesn't set one. The function 0x1939bb0, which calls the camera and `chr_frame_update`, runs only while the game loads.

Not resolved: what ends the fast phase. Candidates are the hunter's behaviour scripts (Havok Script, run outside the updates above) and a timer on the stick input. The next step is a run that catches the write that starts the slow phase.

One 60 run (`185636`) crashed in the logo sequence with the `GXRenderThread` segfault at guest 0x263b8e7 (#559).

## The chase camera at 60 FPS (#557)

`chase_cam_update` is wrapped, not decompiled (`src/game/camera/chase_cam.cpp`). Every frame-rate dependent part of it is a field of the camera that it reads per call, so a wrapper can set those fields for the call and put them back afterwards. The 20 KB function stays the game's code. The hook calls the original through a trampoline of its prologue (`BB_WRAP`, `bb::hook::original`). At 30 FPS the wrapper only calls the original.

The scaling is per frame rate, not per `dt`: at 60 every call stands for half a call at 30, also when the game passes a smaller step (slow motion, pauses). With c = frame rate / 30:

| Kind | Fields | For the call |
|---|---|---|
| factor k per call | +0x1b8, +0x1c4, +0x1c8, +0x1cc, +0x23c, +0x240, +0x288, +0x330 | 1 − (1 − k)^(1/c) |
| kept fraction f (pitch pull-back, 0x143cef0) | +0x1a8 | f^(1/c) |
| turn per call (lock-on stick, added to the angles at 0x143ca30 without dt) | +0x280, +0x284 | / c |
| factor eased by 1/30 a call (0x143e2b2), or reset to +0x1a0 / +0x1b4 by a wall slide (0x143e271) | +0x198 (read before the ease), +0x1ac (read after it, also in a reset call) | set so that the factor the game uses is the scaled one; +0x1b4 scaled for the call; the game's camera collision byte (0x5127a94, tested at 0x143d6d2) tells beforehand whether the call eases. Afterwards the ease over 1/c of a call, the reset value, or the own value when the call skipped the ease |
| weight that pulls the lag factors toward 1 (0x143e7da) | +0x130, set to 1 by camera input, held for +0x134 s, dropped by dt / +0x1e4 before its use | set so that the dropped value is the scaled one; afterwards 1, the own value or the dropped one |

What stays approximate: a factor that the game changes within a step (the ease, the weight's drop) is sampled twice a step at 60 and once at 30, and the lag factor is multiplied by a ramp the call computes (0x143e7d6). In the unit test of the wrapper the two-calls-at-60 result is within 1/5 of the original's 60 FPS error for these, and exact for the rest.

**Check runs.** Branch build, scenario `fr558s` (frame-locked side walks of 10, 30 and 60 steps, 45 steps of rest each), nearest-run picture difference to the 30 FPS runs (`194619`, `194948`, `224203`; stamps `20261115-…`, UTC):

| Picture | 30 vs 30 | 60, unpatched (`194801`, `195131`) | 60, camera every second call with twice the step (`205343`, `205526`) | 60, wrapper (`225033`, `225215`, `225357`) |
|---|---|---|---|---|
| end of the 10-step walk | 1.97 | 7.69 | 3.35 | 3.92 |
| 45 steps later | 2.94 | 8.99 | 8.96 | 9.04 |
| end of the 30-step walk | 4.83 | 11.90 | 8.24 | 6.69 |
| 45 steps later | 5.51 | 9.89 | 8.54 | 8.52 |
| end of the 60-step walk | 4.22 | 10.26 | 4.92 | 4.10 |
| 45 steps later | 1.81 | 6.18 | 2.81 | 2.49 |

The camera now does at 60 what it does at 30, as far as these pictures show: the 60 runs land as close to the 30 runs as running the whole camera at 30 updates a second does. The difference that remains after the rest of the 10-step walk (about 9 against 3) is the hunter's own motion, traced in #558. At 30 nothing changes: the wrapper calls only the original, and `rig.py compare` of `main-menu` (branch `224904` against `main` `224812`, `224838`) gives ratio 0.996 and time ratio 1.006.

**The camera turn.** A frame-locked 0.5 s right-stick turn (scenario `fr557t`, the turn part of `fr558f`) agrees between the rates with or without the wrapper: three 30 runs (`224643` and two of `main`) are 2.8 apart at their nearest, the 60 runs 3.1 (unpatched) and 3.0 (wrapper). The 30 runs fall on one of two outcomes, as with the side walks. Against only two 30 runs of the same outcome the 60 runs looked 7 away; take turn checks over at least three runs per rate.

## The Havok world step (#553)

`havok_world_step` is replaced (`src/game/havok/world.cpp`): the same calls, with `frame_step()` as the step record's delta time. At 30 it is the game's 1/30.

**The world.** The world the step drives is a world of Havok's newer physics, not an `hkpWorld`: the object the stages get (`[[manager+0x28]+8]`) holds a `hknpGroupCollisionFilter` (its type name in the filter's data), and a motion buffer of 4000 entries of 0x80 bytes at +0xe0 (centre of mass at +0, orientation at +0x10, linear velocity at +0x40; entry 0 is the static motion). The step record is 0x18 bytes: delta time, 4 bytes the game never writes, the manager's +0xc8, the thread pool's thread count + 1, and 0. The manager also checks its job queue's critical section (Havok's `lockMutexWithSpinCount` and `leave`, on `pthread_mutex_*`), runs the thread pool on the queue, calls 0x1e42140 and, after the stages, 0x43b0a0 on the queue. The manager's +0x30 holds the step (it calls 0xb50410 on the world's +0xd8 instead), and +0x26c switches it on. Before the step it hands a per-frame counter of the singleton at 0x553b168 out in units of 200 (0x15a5fc0 per item) and calls 0x17fd220 and 0x1806e90; none of it takes a time.

**Not replaced: `hkp_world_cinfo_init`.** Its +0x70 is the `expectedMinPsiDeltaTime` of an `hkpWorldCinfo` (the defaults around it are Havok's: 200 at +0x68, 250 at +0x6c, solver values 0.6, 1, 4, 1 at +0x88 to +0x94). It configures worlds of the older physics, which this step does not drive. A local `bb-run` that logged its calls and wrapped the creator of one such world (0x10588c0) saw neither run from the boot to standing in Central Yharnam, so changing it would change nothing that we can check. The other stepper that calls the same stages (0xa7a250, slot +0x68 of the vtable 0x5297270) is overridden with an empty function in the only class the game builds from it (vtable 0x53a6b00, made at 0x1e155f9).

**Tests.** `tests/game/havok_test.cpp` runs the original and the replacement from the same state through the harness. The issue planned a real world on the full load set. The test hooks the step's callees instead (the two stages, 0x43b0a0, 0x1e42140, 0xb50410, 0x17fd220, 0x1806e90 and 0x15a5fc0) with fakes that record their arguments in the arena, so the harness compares the step record the stages get, the calls and their order, besides every byte the step touches. The stages themselves are Havok's code and stay the game's in `bb-run`. The thread pool is a fake object, and the job queue's mutex is one guest mutex shared by every run. 17 cases cover the step, the held and switched-off manager, missing objects and the counter's paths. Three of them have a fake change the manager during the call (the held byte, the job queue, the world), which pins down when the step reads those fields. At 60 the record's delta time is 1/60 (bits `0x3c888889`) where the original keeps 1/30. The mutex calls are our HLE's and not logged, and the mutex is never contended. So the tests don't see the lock: neither a lock that is left out nor the spin after a failed trylock nor the failure paths (printf, perror, `int3`). Those are for the disassembly review.

**Check runs.** Central Yharnam from the save of the `yharnam-route` runs (`20261115-132906`), scenario `settle` (load, stand 20 s; local, as below), `relwithdebinfo`, RADV, `--window`, 2026-11-16. A local `bb-run` (not committed) logged each moving motion of the world before every step: its centre of mass and velocity. When the street loads, its ragdoll corpses (about 450 bodies) fall and settle without any input. Two runs per variant, at 30, at 60, and at 60 with the step record at 1/30 (as on `main`), runs `015517` to `020238`.

Free fall, the vertical acceleration of the bodies in their first five steps of motion (Δvy per step × frame rate, falling samples, median and quartiles; the share within 0.5 m/s² of g and within 1 m/s² of 2g):

| Variant | Acceleration (m/s²) | at g | at 2g |
|---|---|---|---|
| 30 | −9.04 [−9.72, −4.56], −9.10 | 22 % | 1 % |
| 60 | −9.54 [−11.04, −7.42], −9.55 | 32 % | 1 to 2 % |
| 60, step 1/30 | −17.95 [−19.44, −9.25], −17.83 | 2 % | 22 % |

With the step at 1/60 the world runs at game speed: bodies fall at g at both rates. With the game's 1/30 they fall at twice g in game time, and the corpses settle exactly twice as fast: matched by position, every body reaches half and 90 % of its drop after 0.50 of the time at 30.

The corpses as a whole still settle faster at 60 with the fix: half of their drop after 0.133 s from their first move, against 0.200 s at 30 (0.100 s with 1/30), and they stop moving after 3.7 to 4.1 s against 5.5 to 5.7 s (median over the bodies). The positions show why: the game places each corpse by teleporting its bodies twice after a fixed number of frames (steps 7 and 15 at 30, 6 and 14 at 60), and the collapse after that is faster at 60. That is outside the world step; #567 traces it.

**30 FPS is unchanged.** `rig.py compare` of the picture after the 20 s stand, the branch at 30 (`020950`) against two runs of `main` (`020652`, `020821`): noise floor 0.610, ours 0.414 (ratio 0.68), time ratio 1.002. The branch at 60 in the street (`021118`): 60.00 presents a second, p50 16.67 ms, p99 16.73 ms, no GPU fault. The machine was quiet for these runs (no other test load; CPU load average 1 to 5).

## The AI step (#554)

`chr_ai_step` (0x1e2bc10) runs once a frame from `chr_frame_update` (0x1548740, call at 0x154972c) with the `SprjWorldNvmManager` (0x5540208). Its only other caller is a step of the test module `ModuleNaviMeshTestStep` (0x19baa40, call at 0x19baf55); its call of the AI world step returns to the same address, so it gets `frame_step()` too. Its only use of 1/30 is the task data it builds on its stack (0x1e2bcd1) for one call at its start: `ai_world_step` (0x1dcdad0) on the AI world manager (`SprjHkAiManager` +0x10). That step reads nothing of the task data but `delta_time`, which it passes in `xmm0` to the Havok AI world's step (0x8a0790, a thunk of 0x8a07a0, which takes the manager's +0x78 and +0x80 when +0xf0 is set). The rest of `chr_ai_step` (a loop over the world's +0x2bc8 entries under its third argument; under its fourth, calls with `RendMan` and `WorldChrManDbg` that look like debug drawing) takes no step.

**Wrapped at the call, not decompiled.** `ai_world_step` is wrapped (`src/game/ai/world_step.cpp`, `BB_WRAP`). The hook enters the wrapper with a `jmp`, so its return address is the game's call site: when that is `chr_ai_step`'s call (return address 0x1e2bceb) and the frame rate is not 30, the wrapper hands the original a copy of the task data with `frame_step()`. Every other call keeps its task data: the only other call is in `hkai_test_module_step` (0x19a6860), a step of the test module `ModuleHavokAiTestStep`, which passes on the task data it gets from the task tree. A thunk in padding at 0x1dc91b0 (`mov rdi, [rdi+0x10]; jmp 0x1dcdad0`) has no references; were it used, its tail jump would leave its caller's return address, never 0x1e2bceb. This changes the one value and leaves the 2.8 KB of `chr_ai_step` the game's code. At 30 the wrapper calls the original unchanged.

**Left as it is: the section sweep.** `ai_world_step` hands sections whose bounds changed (flag 4 at +0xc0 of a section object) to the world step in slices of a thirtieth of all sections a call, so one sweep takes 30 calls: 1 s at 30, 0.5 s at 60. That only decides how soon a change is passed on, and at 60 it is sooner.

**Tests.** `tests/game/ai_test.cpp` hooks the Havok AI world's calls in the step (0x8c8360, 0x897390, 0x895bd0 and 0x8a07a0; 0x8c8350 and 0x8a0790 are thunks too short to hook) with fakes that log their arguments and the contents of the step's arrays (they live on its stack, whose address differs with the wrapper's frame). The harness compares the original and the wrapper called directly at 30 and at 60 (both keep the caller's task data), and `chr_ai_step`, run as the game's code up to its call of the step, without and with the registered hook at 30: every byte is the same. At 60, through the hook, the Havok AI world gets 1/60 (bits `0x3c888889`), and without it 1/30.

**Check runs.** Scenario `aiwait` (local): Continue on the save of `yharnam-fight` (the hunter under the arch, a group of living enemies in the street), then stand 30 s while the enemies walk up. A local `bb-run` (not committed) logged per AI world step the Havok AI character (`[agent+0x230]`) of each agent in the manager's list (+0x30): its position at +0x20 and velocity at +0x40. Two runs per variant, at 30, at 60, and at 60 with the old 1/30 (`20261116-024549` to `-025402`, suffix `-bb-run-aiwait`), all finished, no GPU fault, 57 to 58 presents a second at 60 with the log on. Agents matched by their start position, game time from the first AI step:

| | 30 | 60 | 60, step 1/30 |
|---|---|---|---|
| agents | 17 | 16 (one that starts at 10 s at 30 does not come) | 16 |
| when the agents appear | 0 to 0.8, 5.0 to 5.3, 10.1 to 12.9 s | 0 to 0.4, 5.0 to 5.1, 11.1 to 13.2 s | 0 to 0.4, 5.0 to 5.1, 10.9 to 12.7 s |
| first move after appearing | 0.2 to 2.2 s | 0.2 to 2.2 s | 0.2 to 2.2 s (one 1.5 s) |
| median walking speed (0.5 s windows) | 0.81 to 1.10 m/s | 0.70 to 1.06 m/s | 0.79 to 1.08 m/s |
| distance in the first 10 s of motion | 8.6 to 13.6 m | 4.7 to 12.0 m | 6.2 to 12.0 m |

The enemies approach at 60 as at 30: they appear and start in the same game time and walk at the same speed. The old step makes no difference: the 60 runs with and without the fix agree as closely as the runs of either. The game moves its characters itself (their animation), and the Havok AI character's position and velocity are inputs to the step (the velocity's vertical part is −g × the frame's step: −0.327 m/s at 30, −0.163 at 60 in both 60 variants); what the step computes from them did not change the motion in this scene. Five agents (four of the group that appears at 5 s, one of the first) walk 2 to 7 m less in their first 10 s of motion at 60 in all four 60 runs, with or without the fix, so something else that runs per frame holds them; not traced (candidates are the target bank, #555, and the other sites above).

**30 FPS is unchanged.** The save has two outcomes at 30: mostly the enemies pass the hunter by, sometimes one notices and attacks him (picture difference about 21 between the two). In nine runs of `main` (`025542` to `032231`) it attacked once, in nine runs of the branch's code (seven of the branch, two of the probe build) twice; between runs of the same outcome the pictures agree to 0.1 to 0.6. `rig.py compare` of the branch (`031118`) against two runs of `main` with the common outcome (`031254`, `031607`): noise floor 1.108, ours 1.143 (ratio 1.03), time ratio 1.00. At 60 in this scene the branch (`030031`) runs at 57.9 presents a second (p50 16.70 ms, p99 18.80 ms), as `main` does at 60 (`032430`: 57.3, 16.91 ms, 18.96 ms). No run had a GPU fault. The machine was quiet (no other test load, CPU load average 1 to 4).

## Open questions

- Mode 2's `tearing` = 1: where `graphics +0x104`, 0x513ac84 and the render list's byte at +8 end up (presumably the hsync flip when late).
- Whether the Havok behaviour and cloth updates take the task data's step.
- Whether the step-machine functions with task data of 1/30 read `delta_time`.
- `skip_wait` (+0x273, set by 0x1cf4c70 with `SprjScaleform`) and `request_0x274` (sets +0x4c, +0x4d and 60 at +0x50 of the object at 0x5540e00): what state skips the wait.
- The remaining uses of 1/30 and 30.0f that the survey did not classify.
