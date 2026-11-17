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
| 0x180f647 | `chr_list_fade_step` 0x180f5e0 | a fade `+0x1c0 = min(fade + 1/30, 1)` per call over the character list (#556) | fades in 0.5 s instead of 1 s |
| 0x143e2b2 | `chase_cam_update` 0x143ac60 | the player's chase camera eases its follow weight `+0x198 += (+0x19c - +0x198) / 30` per call (first read as a `RendMan` smoothing; see #558 below) | one of the camera's per-call smoothings |
| 0x173456e, 0x15dc1f7, 0x1044943, 0x1f77d4a, 0x227784b, 0x18bc4f7 | the shop's tab highlight (`Ins_tab_select`), the effect system, and four that run once or not at all (#556) | an `update(1/30)` call with a literal | see "Fades, effects and menus (#556)" |

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

No stutter in play. The clinic holds an even 16.7 ms median; in `play-stand` and `play-walk` about 1 to 5 % of the frames miss their slot by 2 ms (18.7 ms), as in #509's check run. They came from the load on the machine, not from the flip grid ("The 18.7 ms intervals" below, #605). A fourth run at 60 (`-161738`, busy part) ended in `play-dodge` with a segfault of a `GXWorker` thread at guest 0x263b8e7, reading 0x4300000000 while unlinking from a list; the same address faulted in a 30 FPS `main-menu` run of `main` on 2026-11-12 (`20261112-133325`), so it is older than this change, and a frame rate twice as high may only make it more likely. The main menu at 60 (`-163245`): 60.0 presents a second, p50 16.67 ms, p99 16.72 ms.

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

- **Havok world step** (`havok_world_step`, 0x17f99d0): done in #553, see "The Havok world step" below.
- **Ragdolls** (`chr_physics_pose_sync`, 0x1515ec0, a literal 1/30 for the ragdoll updates): the step fixed in #567, see "The ragdolls" below. The corpses still stop about 0.3 s later and come to rest about 0.3 m from where they do at 30; the cause is not isolated (#567 stays open for it).
- **AI step** (`chr_ai_step`, 0x1e2bc10): done in #554, see "The AI step" below.
- **Target bank** (`target_bank_scan_tick`, 0x12a43b0, and the ring's 30 slots a second at 0x12ab623): done in #555, see "The target bank" below.
- **Fades, effects and menus** (`chr_list_fade_step`, 0x180f5e0, and the `update(1/30)` calls 0x173456e, 0x15dc1f7, 0x1044943, 0x1f77d4a, 0x227784b, 0x18bc4f7): done in #556, see "Fades, effects and menus" below.
- **Chase camera** (`chase_cam_update`, 0x143ac60, 0x4e6c bytes): done in #557, see "The chase camera at 60 FPS" below.
- **AI steering** (`agent_steer`, 0x1dbbd00, its ring of 30 directions and a literal 1/30): done in #570, see "The steering ring" below.
- **Character update intervals** (`world_chr_update`, 0x19173b0, an interval in frames): done in #573, see "The character update interval" below. Two agents of the `aiwait` scene that start right after the load still differ by 0.3 to 0.6 m in their first 10 s (#573).
- **`chr_pad_step`** (0x1572900) is not the player's input: its 1/30 goes to 0x1d145b0, which turns a matrix by the pad's axes times π/2 and −π/3 rad/s, a debug free camera behind a mode check. Low priority.
- **The offset of camera and hunter** of the check runs above: the rig's inputs, the chase camera and the game's input latency in frames, see the next section. The latency is shorter at 60 and stays that way.


## Camera and hunter at 60 FPS (#558)

The 30-vs-60 picture difference of #514's runs has two causes: the way the rig's scenario times its inputs, and the chase camera's per-call smoothing. None of the known 1/30 sites (#553 to #556) is involved. What remains in the hunter's own motion is the game's input latency, two frames at either rate, so 33 ms shorter at 60 ("The input latency" below). It is kept.

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

What ends the fast phase is the stick's release: see the next section.

### The input latency

A local `bb-run` also logs, in the same row as the hunter's position, the left stick as the game last read it and the number of pad reads. The game reads the pad once a frame, one sample at a time (the current state), so the log shows on which frame the game sees a press and a release. Scenario `fr558s` on `main` at d80a2df, six runs at 30 and four at 60 (`20261117-0323…` to `-0346…`, UTC), CPU load average 2 to 4, no other game runs.

**The holds.** The rig's `frames N` hold covers N game reads give or take one: the first 10-step hold reached the game as 9 reads in all six 30 runs, the second as 9 or 10, the 30-step holds as 29.5 to 31 steps of 1/30 s at both rates. So two runs of the same scenario hold the stick for different game times, which is the bimodal noise of the earlier tables. Distances below are compared at the same number of reads.

**The latency.** From the frame that reads the press, the hunter turns and starts to move two frames later at both rates: 67 ms at 30, 33 to 50 ms at 60 (the first motion is small, so one frame either way). From there the start ramp has the same shape in game time, the steady speed is 4.0 m/s at both rates, and the release stops the fast phase on the frame that reads it, at both rates (that is the "frame without motion"). So at 60 the hunter moves for about 1/30 s longer on the same hold: distance from the start, at the release and 45 steps (30) later:

| Hold seen by the game | 30 FPS | 60 FPS |
|---|---|---|
| 9 steps of 1/30 s | 0.291 to 0.292, 0.394 to 0.400 (6 runs) | 0.423, 0.520 (1 run) |
| 10 steps | 0.415, 0.517 (4 runs) | 0.545 to 0.556, 0.643 to 0.664 (4 runs) |
| 30 steps | 2.237 to 2.342, 2.343 to 2.449 (5 runs) | 2.572, 2.680 (2 runs) |

At 60 every hold ends 0.13 m further than at 30 for the same game time, and 0.23 to 0.34 m further after the 30-step walk, which has a second transition (from the start step into the run, 0.6 s in) that again takes effect a frame later at 30. The 60-step walks end against the shelf at both rates. At 30, two runs whose holds differ by one read differ by the same 0.12 m, so the 30-vs-60 difference is one 30 FPS frame of input timing, not a speed or a drift: between the transitions both rates move at the same speed.

**The check.** With the probe showing the stick's press to the game two reads late at 60 (`BB_PADLAG=2`, run `033619`; the rig held 20 reads, the release is unchanged), the press reaches the hunter four frames after it was read, 67 ms as at 30, and the first 10-step side step ends where the 30 runs with a 10-read hold end: 0.423 m at the release and 0.532 m after the rest, against 0.415 and 0.517 (unpatched 60: 0.556 and 0.664). The remaining 0.008 to 0.015 m is less than one 60 FPS frame of motion (0.067 m at 4 m/s).

**Kept.** The latency is the game's own: it counts frames, and a frame is shorter at 60. Adding two reads of delay at 60 would make the positions match 30 but give the player 33 ms more input lag, the opposite of what 60 FPS is for. No site is patched and no decomp issue follows from it. The camera follows the hunter, so the offset shows in the pictures (7.5 to 9.9 against the nearest 30 run in "The hunter's side step" above). It comes from the transitions, so it does not grow with the length of a walk.

**Not traced:** in one of 24 starts at 60 (and in two of the runs with the delayed press), never in 36 at 30, the start ramp after a rest skips about two frames of its latency and the hunter ends 0.2 m further. All of these started from the rest after an earlier step, so the hunter's state at the press may differ. Low priority.

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

The corpses as a whole still settle faster at 60 with the fix: half of their drop after 0.133 s from their first move, against 0.200 s at 30 (0.100 s with 1/30), and they stop moving after 3.7 to 4.1 s against 5.5 to 5.7 s (median over the bodies). The positions show why: the game places each corpse by teleporting its bodies twice after a fixed number of frames (steps 7 and 15 at 30, 6 and 14 at 60), and the collapse after that is faster at 60. That is outside the world step; see "The ragdolls (#567)".

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

The enemies approach at 60 as at 30: they appear and start in the same game time and walk at the same speed. The old step makes no difference: the 60 runs with and without the fix agree as closely as the runs of either. The game moves its characters itself (their animation), and the Havok AI character's position and velocity are inputs to the step (the velocity's vertical part is −g × the frame's step: −0.327 m/s at 30, −0.163 at 60 in both 60 variants); what the step computes from them did not change the motion in this scene. Five agents (four of the group that appears at 5 s, one of the first) walk 2 to 7 m less in their first 10 s of motion at 60 in all four 60 runs, with or without the fix, so something else that runs per frame holds them: the steering ring (#570, below).

**30 FPS is unchanged.** The save has two outcomes at 30: mostly the enemies pass the hunter by, sometimes one notices and attacks him (picture difference about 21 between the two). In nine runs of `main` (`025542` to `032231`) it attacked once, in nine runs of the branch's code (seven of the branch, two of the probe build) twice; between runs of the same outcome the pictures agree to 0.1 to 0.6. `rig.py compare` of the branch (`031118`) against two runs of `main` with the common outcome (`031254`, `031607`): noise floor 1.108, ours 1.143 (ratio 1.03), time ratio 1.00. At 60 in this scene the branch (`030031`) runs at 57.9 presents a second (p50 16.70 ms, p99 18.80 ms), as `main` does at 60 (`032430`: 57.3, 16.91 ms, 18.96 ms). No run had a GPU fault. The machine was quiet (no other test load, CPU load average 1 to 4).

## The target bank (#555)

`target_bank_scan_tick` (0x12a43b0) is a member functor of a per-frame task (built at 0x1a654b8) on the `SprjTargetBankManager` singleton (0x553b098, built by 0x12a3dd0). It does two things per call, neither with the frame's step:

- **The bank.** A timer (+0x2c) goes up by a literal 1/30 (0x12a4416). When it reaches the interval (+0x28, 5 s; the timer starts at 5, so the first call scans), the tick resets it and rebuilds the bank, a vector of characters at +0x08 to +0x18: every character of `WorldChrMan`'s list (+0x30, +0x38) and of the chr set the singleton at 0x553b148 names (+0x20) whose virtual +0x78 says yes. Between rebuilds the bank only loses characters that are destroyed (0x1a554f0, 0x18bc110). It is read by 0x122b3f0 and 0x12adc50. So a character that appears waits for the next rebuild, up to 5 s, before it is in the bank.
- **The AI update schedule** (debug label `SprjAiUpdateTimeSchedular`). A ring of 100 byte counters at +0x30 with its index at +0x94: each call clears the slot at the index and moves the index on by one. The AI's think update (0x12ab4c0) books its next think 1.0 to 1.2 s ahead (a random time) in the slot `seconds * 30` past the index (0x12ab623). A slot holds at most +0x98 (3) bookings; when it is full, the booking moves on to the next free slot and the think time grows by 1/30 s per slot skipped. `Disable System` (+0x99) switches the limit off. The think timer itself counts seconds; the ring only spreads the thinks over the frames.

At 60 the tick ran twice as often per game second: the bank was rebuilt every 2.5 s, and each slot of the ring stood for 1/60 s where the bookings count it as 1/30 s.

**Wrapped, not decompiled.** The hook (`src/game/ai/target_bank.cpp`, `BB_WRAP`) runs the game's tick on every (frame rate / 30)-th call: every call at 30, every second call at 60. The timer, the rebuilds and the ring then move on 30 times a game second, as at 30 FPS, bit for bit the same sequence of values. The index stays on one slot for two frames at 60, which is what the bookings' `seconds * 30` assumes. The bank's readers see it rebuilt on the first frame of a pair, at the same game time as at 30. The phase of the pairs is a counter in our code, arbitrary at the start; the tick takes no task data.

**Tests.** `tests/game/target_bank_test.cpp`: the game's tick through the registered hook at 30 leaves every byte as without it (1 and 160 calls), and at 60 twice as many calls through the hook leave what the game's tick leaves in the same number of calls at 30: the timer, the bank and the ring. Fakes stand in for the world's characters (a vtable whose +0x78 reads a byte) and the two singletons; the bank has room for every character, so it never grows. Four cases: the game's own state at creation, a short interval with the ring wrapping from 99 to 0, and no player chr set (-1 and one past the end). Without the wrapper the 60 FPS calls differ (a test checks that the comparison sees it).

**Check runs.** Scenario `aiwait` (#554, local) on the `yharnam-fight` save: Continue, then stand 30 s while the street's enemies walk up. A local `bb-run` (not committed) logged every call of the tick: the slot it clears and its count, the timer, the bank's size and the ring. `relwithdebinfo`, RADV, `--window`, 2026-11-16, runs `20261116-035925` to `-041132` (suffix `-bb-run-aiwait`); CPU load average 2 to 5 from other jobs, no other test load. Game time is calls / frame rate:

| Variant | Runs | Bank rebuilt every | Ring slots a game second | Bookings a game second | Full slots cleared a game second |
|---|---|---|---|---|---|
| 30 | 2 | 5.03 s | 30.00 | 21.7 | 0.92, 1.24 |
| 60 | 2 | 5.03 s | 30.00, 30.01 | 22.8, 23.0 | 1.52, 1.46 |
| 60, the old tick on every call | 2 | 2.52 s | 60.00 | 23.0, 23.1 | 0.50, 0.61 |

The bank is rebuilt at the same game times at 30 and at 60 (0, 5.03, 10.07, ... s) and holds the same characters (34 at the start, 29 to 30 after 35 s). The ring moves on at 30 slots a game second again, and its slots fill up again: full slots are back near the rate at 30, where with 1/60 s slots they rarely were. The rest of the gap to 30 fits the AI booking about 5 % more thinks at 60 (below). The AI books about 5 % more thinks a game second at 60 with or without the fix; that comes from the think update and not from this tick. One 60 run (`040102`) lost the menu taps and stood at the title; it is left out.

**30 FPS is unchanged.** `rig.py compare` of the three pictures of the stand, the branch at 30 (`041254`, `041742`) against two runs of `main` (`041430`, `041606`): noise floor 1.199, ours 1.205 and 1.243 (ratio 1.005 and 1.037), time ratio 1.00.

## The steering ring (#570)

With #555 in `main` the five enemies of the `aiwait` scene still walked 2 to 4 m less in their first 10 s of motion at 60, and a sixth that walks alone (it starts at (−225, 63)) walked at 0.82 m/s instead of 1.08. They stop for 0.5 to 2 s and turn on the spot where at 30 they walk on.

**Found by halving.** A local `bb-run` ran one game function per object only every second call at 60, with twice the step (in `xmm0`, and in the task data where the function takes one), and logged the AI agents' positions as in #554. Halving `chr_frame_update` (0x1548740), the animation update (0x15af4f0, 0x15b2910), the AI agent update (0x12a85c0) or the Havok world step, alone or all together, left the shortfall. Halving the AI manipulator update (0x151a720, virtual +0x48 of a character's manipulator, called per character on the `CSChrThread`s by 0x1513c10) removed it, and so did, step by step inside it, the AI character update (0x128f060) and in that the path update (0x1234750): there the six agents walked 9.6 to 13.9 m against 9.7 to 13.6 m at 30, at the speed of 30. A log of the path update showed the cause: at 60, for exactly these agents, the point they steer at is their own position once every 30 calls (0.5 s), never at 30. Each time the path update's two stuck checks (+0xb0 to +0xd9, 1 s and 3 s windows of distance walked, frame-rate independent) start over and the manipulator gets no direction for a frame.

**The ring.** The point comes from `agent_steer` (0x1dbbd00), virtual +0x50 of the AI agent (vtable 0x534e220; the agent whose Havok AI character is at +0x230), which the path follower (0x1239eb0) calls with the frame's task data. It stores the step at +0x280, sets +0x28c (the AI world step, 0x1dcde52, reads and clears it), runs a small state machine (+0x258, timer +0x25c) and writes the direction to the next path point into a ring of 30 slots (+0x30, 16 bytes each, write index +0x24c, modulo 30 at 0x1dbc2d4). It returns the character's position plus 3 times the average of the slots from a read index (+0x248) up to the newest, and keeps that direction at +0x20. The read index lags at most `+0x250 × (1/30) / delta_time` calls behind (0x1dbc2fd); +0x250 is a lag in 30 FPS frames that the agent's setter (0x1dbbc69) computes from the speed, 1 to 29, high when the agent walks slowly. At 30 the lag fits the ring. At 60 it is twice as many calls, and from 15 on it reaches the ring's size: the read index meets the write index once a round, the average is empty, and the point is the agent's position. The state machine's state 1 also counts its timer down by a literal 1/30 a call (0x1dbc5cc), and the setter turns the stored step into a look-ahead distance (0x1dbbc8f).

**Wrapped, not decompiled.** `agent_steer` is wrapped (`src/game/ai/steering.cpp`, `BB_WRAP`). At 30 the wrapper calls the original. At 60 it counts each agent's calls and calls the original on every second one, with the task data's step doubled (2 × 1/60 is the game's 1/30 bit for bit), so the ring, the lag, the timer and the stored step move on as often a game second as at 30. On the other calls an agent with a Havok AI character gets two of the steering's writes on every call (+0x28c = 1, the character's +0x64 to +0x1c of the object at `[[agent+0x238]+0x58]`) and the point the original returns, built the same way from the character's position now and the direction at +0x20 (0x1dbc4d2 to 0x1dbc529: multiply, then add, no fused multiply-add). The third write, clearing the requests at +0x289 and +0x28a (0x1dbc52c), is left out: only the steering acts on them (set by 0x1dbb990, read at 0x1dbc69a), so they wait for the next call that runs it. An agent without a character gets the original, which returns a constant, and its count starts over, so its first call with a character runs the original.

The count is per agent, not per frame: the path follower doesn't step every agent every frame. A log of the steering at 60 showed agents called on every frame, on every second and on every fifth. A first version that ran the original on even frames left agents stepped every second frame on odd frames only: one ran the steering 16 times in 919 calls, five enemies of the scene stood still and one ran at the hunter. The counts live in a table by the agent's address under a lock (the agents are stepped on several `CSChrThread`s); the table starts over when it holds 4096 agents, which costs each agent at most one extra run.

**Only agents of characters with interval 0 (#603).** Since #573 a character with an update interval u > 0 is updated every 2u frames at 60 with a step of 2u/60 s, as often a game second as at 30 and with the same step. `agent_steer` is reached only from inside that update (0x18bdb20, then `chr_ai_control_update`, 0x1513c10), so halving it too ran these agents at half their rate at 30. The step in the task data can't tell the two cases apart (0x1a38430 scales it in hit-stop), so `chr_ai_control_update` is wrapped as well: it notes the character's interval for the thread while it runs (its second argument is the character's control object, the character is the pointer at +8 of it, the interval the int32 at +0xe4 of the character), and `agent_steer` halves only when no character is noted (a call from elsewhere) or the interval is 0. Otherwise it calls the original with the caller's task data and sets the agent's count to 0. The note is a `thread_local` saved and restored around the call, because the game steps an agent on the thread that updates its character. At 30 the wrapper only calls the original.

**A fresh agent runs at once.** The counts are kept by the agent's address, so a new agent at the address of a destroyed one inherits its count and may skip its first call, which then returns its own position plus a direction the steering never computed. The agent's constructor (0x1dbae40, store at 0x1dbaf4b) sets the stored step (+0x280) to 0.0f and only `agent_steer` writes it (0x1dbbd45), so a step of 0.0f means the steering has not run on the agent: the wrapper runs it now whatever the count says, and the count goes on from there, so the next call is the skipped one. But a run with a step of exactly 0 stores 0.0f too, and in a full hit-stop (0x1a38430) the step is 0 for several frames, so 0.0f alone would make the wrapper run the original on every call. The wrapper therefore keeps, next to the count, whether the last run it passed on had a step of 0, and a 0.0f then counts as fresh only if that was not so. A new agent at an address whose previous agent last ran with step 0 just follows the inherited count.

**Check runs (#603).** Scenario `aiwait` (#554, local) on the `yharnam-fight` save, `relwithdebinfo`, RADV, `--window`, 2026-11-17, with a local probe that logs every steering call (the noted interval, the step, whether the game's steering ran) and can switch back to halving every agent. Three runs at 60 with the fix (`151752`, `151927`, `152102`), two at 60 with the old halving (`150421`, `150556`), four at 30 (`145621`, `150939`, `152237`, `152413`; stamps `20261117-…`, suffix `-bb-run-aiwait`), all with the save's common outcome; CPU load average 1 to 3, no other test load. About 19,500 steering calls a run, all from inside `chr_ai_control_update`: about 16,400 with interval 0 (step 1/60), 2,500 to 2,900 with interval 2 (step 4/60) and 300 to 450 with interval 5 (step 10/60). With the old halving the game's steering ran on half the calls of every interval; with the fix on every call of intervals 2 and 5 and on half of interval 0. Ten agents a run got their first call through the fresh rule. In this scene the motion hardly shows it: the distances in the first 10 s of motion differ by at most 0.1 m between the fix and the old halving for every agent (13 and 14 of 17 within 0.2 m of 30; the agents that are off are those of #573), and the patrol stops are as long. The one difference is when the patrol at (−214.8, 104.5) stops: its third and fourth stops start at 27.4 and 36.5 s with the fix and at 27.7 and 36.9 s with the old halving, against 27.2 and 36.3 s at 30. At 60 the fix runs at 59.0 to 59.1 presents a second (p50 16.67 ms, p99 16.71 ms), as before; at 30 the tracks equal the earlier 30 runs as closely as those equal each other, 29.7 presents a second, p50 33.33 ms. No GPU fault.

**Tests.** `tests/game/steering_test.cpp` runs the game's steering on an agent of the game's class whose character is in state 3, so the steering takes the direction to the next point of a path (the game's 0x1dbca40 and 0x89c030 on a path in the arena), with lags of 5, 20 and 29 frames, straight and bending. Through the registered hook at 30 every byte and every returned point is the game's (120 calls). At 60, 240 calls through the hook leave what 120 calls of the game's steering leave at 30: the ring, the state, the step, and the points, the second call of each pair returning the point of the first. Without the wrapper the game's steering at 60 returns the agent's own position once every 30 calls with a lag of 20, at 30 never, and through the hook at 60 never. Tests of `agent_steer_every` with a stand-in check the call pattern, an agent without a character, and the point and writes of a skipped call.

**Check runs.** Scenario `aiwait` (#554, local) on the `yharnam-fight` save, with the #554 probe logging each agent's Havok AI character per AI step, and at 60 a log of every steering call. `relwithdebinfo`, RADV, `--window`, 2026-11-16, runs `20261116-075248` to `-075910` (suffix `-bb-run-aiwait`), two at 30 and three at 60; CPU load average 2 to 3 from other jobs, no other test load. In the steering log every one of the 18 agents ran the game's steering on half its calls (within 2). Distance in the first 10 s of motion (m), game time, agents by start position:

| Agent | 30 (2 runs) | 60, branch (3 runs) | 60, `main` (`-063021`) |
|---|---|---|---|
| (−232, 40) | 12.4, 12.4 | 12.8 to 12.9 | 10.3 |
| (−231, 46) | 9.9, 9.7 | 9.6 | 7.9 |
| (−231, 50) | 11.8, 11.8 | 12.1 to 12.2 | 8.8 |
| (−230, 48) | 11.6, 11.6 | 12.0 to 12.1 | 7.3 |
| (−229, 54) | 13.5, 13.5 | 13.8 to 13.9 | 11.8 |
| (−225, 63) | 12.4, 12.4 | 12.8 | 10.1 |

The shortfall is gone: the five enemies and the lone walker walk as far as at 30, at the same median speed (the lone walker 1.08 m/s at both rates, 0.82 on `main` at 60). The three 60 runs agree to 0.1 m. What is left at 60 is small and the same on `main` (#573, mostly the probe's rounding, see "The character update interval"): most agents walk 0.3 to 1.6 m more in their first 10 s than at 30 (the same agents on `main` at 60 already did, for example (−215, 104) 9.8 against 8.8 m and (−192, 83) 9.8 against 8.7), and one enemy that starts at 10 s at 30 does not come at 60. At 60 the branch runs at 57.8 to 58.1 presents a second with the logs on (p50 16.67 ms, p99 18.8 to 19.5 ms), no GPU fault.

**30 FPS is unchanged.** The branch's agent tracks at 30 equal those of `main` with the same probe (`062701` to `063326`) to 0.7 m, the spread between two runs of the same build for the enemies that arrive at 11 s (the others agree to 0.4 m). `rig.py compare` of the three pictures of the stand, the branch at 30 (`080045`, `080221`) against two runs of `main` (`074921`, `075057`): noise floor 1.244, ours 1.196 and 1.205 (ratio 0.96 and 0.97), time ratio 1.00.

## The character update interval (#573)

**Two of the differences were the measurement.** The #554 probe printed the Havok AI character's floats with four significant digits, so positions resolved to 0.1 m, and the distance walked, summed over steps, depended on the sample rate. With seven digits, 11 of the 17 agents of `aiwait` walk the same distance in their first 10 s at both rates (within 0.1 m). The enemy that "does not come at 60" comes at the same time: its start position is (−215.8, 74.50) at 30 and (−215.8, 74.53) at 60, and the track script, which matched agents by the rounded start position, gave it the key of a neighbour at (−215.5, 75.0). Agents are now matched by the nearest start position.

**The patrol stops.** What was left is real: the two enemies that patrol between two points (starting at (−192, 83) and (−215, 104)) stop at each point for 1.97 s at 30 and for 1.57 s at 60, every cycle. At the point they stop, turn on the spot and walk on. The stop and the turn begin and end at the same game time at both rates. At 30 they walk on 0.3 to 0.5 s after the turn, at 60 just before it ends. Halving `chr_frame_update`, the AI character update (0x128f060), the manipulator update or the AI agent update per object (as in #570) left the stop at 1.57 s. A watchpoint on the Havok AI character's facing found its writer, the agent's virtual +0x40 (0x1dbba90), called from 0x12355c0 on the character threads. The facing changes only every fifth frame, at both rates. At 30 the stop is 1.97 s when the agent's facing is updated every fifth frame and 1.50 s when it is updated every second frame.

**The interval.** `world_chr_update` (0x19173b0) runs once a frame from WorldChrMan's frame update (0x1915320, which counts the frames at WorldChrMan +0xa68). It lists the characters due this frame and runs their updates, most of them as jobs on the `CSChrThread`s. Each character has an update interval in frames at +0xe4, set by the frame update from the character's distance: 0 is every frame, and with u > 0 the character is due when its index in its group modulo u equals the frame counter modulo u (0x1917909), with a step of u frames (`vcvtsi2ss` and `vmulss` at 0x1917923). Its own time is right at any frame rate: the elapsed time reaches it as the step (+0xe0). But the number of updates a game second is 30/u at 30 and 60/u at 60. The patrol stop takes a fixed number of a character's updates (about three more than the turn), so at 60 it was shorter. The interval is also a level of detail that the characters' own code tests against fixed values (−1, 0, 2, 5, 30, and a switch over −1 to 5 at 0x18cd1fa), and that `world_chr_update` keeps at [rbp−0xc8] for its counts, so doubling the field itself would change what the characters do.

**Patched inside the function.** The hook framework can now replace instructions inside a function (`BB_PATCH`, `docs/design/runtime.md`, "Hooks"). The 36 bytes at 0x1917909 jump to a stub (`src/game/chr/update_interval.cpp`) that does what they do with k·u in place of u in the two divisions and the step's factor, where k is the frame rate / 30, and keeps u in ecx and [rbp−0xc8]. At 60 a character with interval u is then due every 2u frames, with 2u steps of 1/60: as often a game second as at 30, with the same step. Characters with interval 0 still update every frame. `world_chr_update` is wrapped (`BB_WRAP`) to set k and the stub's two jump targets before the game's code runs. At 30, k is 1 and the stub does what the game's bytes do.

**Tests.** `tests/game/chr_update_interval_test.cpp` enters the game's code at 0x1917909 from an assembly driver with set registers and frame, and two patches at the instructions behind the check (0x191792d, due; 0x1917a1b, not due) record the registers and the frame slot. For intervals 1, 2, 3, 5 and 30, indexes 0 to 39 and frame counters up to 2³²−1: at 30 the stub leaves exactly what the game's bytes leave; at 60 it leaves what the game's bytes leave for 2u, except that ecx and [rbp−0xc8] stay u. Over 6u frames at 60 every character is due 3 times with 2u steps, where the game's bytes are due 6 times. The wrapper sets k from the frame rate. `tests/hook/hook_test.cpp` covers `BB_PATCH`: the patch, its refusals (the function is not an FDE start, the bytes do not lie inside it, differ from memory, are fewer than 14 or more than 64) and overlaps with other entries and live hooks.

**Check runs.** Scenario `aiwait` (#554, local) on the `yharnam-fight` save, with the #554 probe at seven digits. `relwithdebinfo`, RADV, `--window`, 2026-11-16 (suffix `-bb-run-aiwait`): `main` at 30 (`082853`, `083406`) and 60 (`083029`), the branch at 30 (`095010`; `094659` had the save's other outcome, an enemy attacks) and 60 (`094523`, `094835`, `095146`). CPU load average 2 to 3, no other test load. Distance in the first 10 s of motion (m), agents by start position:

| Agent | 30, `main` | 30, branch | 60, `main` | 60, branch |
|---|---|---|---|---|
| (−235.3, 36.6) | 7.7, 7.7 | 7.7 | 7.5 | 7.4 to 7.6 |
| (−234.0, 38.2) | 9.5, 9.5 | 9.5 | 9.4 | 9.4 to 9.5 |
| (−232.4, 40.1) | 10.4, 10.5 | 10.5 | 10.5 | 10.5 to 10.6 |
| (−231.8, 43.3) | 10.1, 9.9 | 10.0 | 11.0 | 10.5 to 10.6 |
| (−230.9, 45.6) | 8.0, 8.1 | 8.1 | 7.7 | 7.7 to 7.8 |
| (−230.6, 50.1) | 10.1, 10.2 | 10.2 | 10.1 | 10.1 to 10.2 |
| (−229.8, 47.5) | 9.6, 9.6 | 9.6 | 9.6 | 9.6 |
| (−228.9, 54.3) | 11.6, 11.7 | 11.6 | 11.6 | 11.6 to 11.7 |
| (−224.8, 63.4) | 10.7, 10.7 | 10.7 | 10.7 | 10.7 |
| (−223.1, 60.1) | 10.0, 10.0 | 10.0 | 9.9 | 9.9 to 10.0 |
| (−215.8, 74.5) | 8.4, 8.4 | 8.4 | 8.4 | 8.4 |
| (−215.5, 75.0) | 8.5, 8.5 | 8.4 | 8.4 | 8.6 |
| (−215.3, 79.1) | 9.5, 8.8 | 8.7 | 8.6 | 8.7 to 8.8 |
| (−214.8, 104.5), patrol | 8.5, 8.5 | 8.5 | 8.7 | 8.5 |
| (−213.4, 76.5) | 8.8, 8.9 | 8.9 | 8.8 | 8.9 |
| (−213.1, 79.7) | 8.5, 8.5 | 8.8 | 8.5 | 8.5 to 8.6 |
| (−192.0, 82.7), patrol | 8.6, 8.6 | 8.6 | 9.0 | 8.6 |

The patrol stops are 1.98 s at 60 as at 30 (1.97 s), in every cycle of every run; their facing changes every tenth frame, as every fifth at 30. Where an agent is on interval 2 at 30 the stop is 1.57 s, and 1.52 s on the branch at 60 (4 frames). Fifteen of the 17 agents walk within 0.2 m of 30 in their first 10 s.

**Left: two agents after the load.** (−231.8, 43.3) walks 0.4 to 0.6 m further than at 30, and (−230.9, 45.6) 0.3 m less. Both belong to the group that is there when the save loads. At 60 that group appears in the AI world 0.2 to 0.4 s earlier after the load and so starts walking earlier (the agents that appear later start at the same game time). From 5.5 s, (−231.8, 43.3) walks shoulder to shoulder with (−234.0, 38.2), 0.65 m apart. At 30 the pair slows it down and turns it, and that part of its track differs from run to run after 7 to 8 s even at 30 (two runs of `main`), though its 10 s distance stays at 9.9 to 10.1 m (also in two runs with the frame counter of the due check shifted by 1 and 3). Halving `chr_frame_update` together with this fix brings the group's start times back to 30, but (−231.8, 43.3) then walks 11.0 m. Stepping the physics world at 30 (`BB_HKHALF`) or the AI world's section sweep at 1/60 a call changed neither. This stays open in #573.

**30 FPS is unchanged.** With k = 1 the stub is the game's check (the tests above). The branch's agent tracks at 30 differ from `main`'s as much as two runs of `main` differ, except for the pair above after 7 s. `rig.py compare` of the three pictures of the stand, the branch at 30 (`095010`) against two runs of `main` (`082853`, `083406`): noise floor 1.082, ours 1.093 (ratio 1.01), time ratio 1.002. At 60 the branch runs at 57.6 to 58.3 presents a second with the log on (p50 16.67 ms, p99 18.8 to 19.2 ms), as `main` does (`083029`: 58.1, p99 18.9 ms), no GPU fault.

## Fades, effects and menus (#556)

**What the sites are.** A gdb probe counted the hits of each site per frame in three runs at 30 (`main` build, 2026-11-16): the title and a load into the clinic with the pause menu and the inventory (`fr556p`, `fr556m`, local), and `yharnam-fight`.

| Site | What it is | Hits | Change |
|---|---|---|---|
| 0x1f77d4a | `sfx_frame_update` (0x1f77bd0), SprjSfx's frame update from `SprjGraphicsStep::STEP_Update` (0x1d9b340): steps the effect system (SprjSfx +0x70, built at 0x1f77250; virtual +0x40 is 0x22ff340, which takes the step in xmm0) by 1/30, or by 0 once after +0x8c was set | once a frame, everywhere | the step: 1/F |
| 0x173456e | `shop_tab_refresh` (0x1734510): updates the shop's (`S02`) tab highlight instance by 1/30 through the layout instances' update (virtual +0x88; 0x0cec810 advances an instance's animations and its children by the step) | not in these runs; by the code once a frame while the shop is open, from its frame update 0x1736140, which has already advanced the whole layout by the task's step; once on an event from 0x1732fe0 and the message handler 0x1733fa0 (message 0x9c9) | the extra update: 1/F |
| 0x180f647 | `chr_list_fade_step` (0x180f5e0): a fade from 0 to 1 by 1/30 a call, copied to the list's parts of kind 7 | 30 adds a fade, in 53 to 97 frames, for three owners while the save loads | the add: 1/F |
| 0x18bc4f7, 0x18bc50d | 0x18bc470, a character's first update (0x18e5520, the character's setup, calls it last, as does the virtual 0x18f2cf0 at 0x18f34d6): 0x18bc980 and the virtual +0x150 with 1/30 | once per character as it is created | none: one step at creation |
| 0x1044943 | 0x10448b0, a virtual (0x52fe4d0) that sets up an object, updates it once by 1/30 and hands it on | none | none: one step at creation |
| 0x15dc1f7 | 0x15dc100, a message handler: on message 0x3ef, 0x15dcdb0 with 1/30 | none | none: once per message |
| 0x227784b, 0x2277874 | 0x2277780, from 0x22c53e0 in the effect system while its +0x2977 is set: 0xebbda0 with 1/30 per list entry | none | none yet: not seen running |

`chr_list_fade_step` runs from a character's physics pose sync (0x1515ec0, through 0x180df90 and 0x1811040), which a second probe at 60 found once a frame for every character, whatever its update interval (#573): 280 calls in 280 frames for each of 30 characters, with intervals 0 to 30. So a call stands for a frame, and the add is 1/F.

**Patched inside the functions.** Each site is a `BB_PATCH` of the few instructions around the literal (`src/game/frame/step_literals.cpp`): the stub does what those bytes do with `bb_frame_step` (1/F, written by `set_frame_rate`) in place of the literal, and jumps back. At 30, `bb_frame_step` is the game's 0x3d088889. The stubs' jump targets are set once by `bb-run` after the image is mapped (`set_step_literal_targets`). The effect patch takes the skip test with it (0x1f77d3b to 0x1f77d52), because the literal's load is the target of no jump but the call behind it is.

**Tests.** `tests/game/step_literals_test.cpp` runs each of the three game functions whole, with fakes for its callees, once with the game's bytes and once with the patch, from the same state: at 30 the harness finds no difference in anything they touch (effects with and without the skip flag; the shop with and without the instance, in both tab modes; the fade at 0, 0.5, 0.98, 0.99, 1, 1.5 and −0.25 with 0, 1 and 4 parts). At 60 the effect system and the highlight get 1/60 (the skip still 0), and the fade reaches 1 in 60 calls where the game's bytes take 30. With 1/60 in place of 1/30 at 30 every comparison fails.

**Check runs.** `relwithdebinfo`, RADV, `--window`, 2026-11-16 (suffix `-bb-run-yharnam-fight`): `main` at 30 (`112920`) and 60 (`113214`), the branch at 60 (`112547`), each with gdb logging the effect system's step (0x22ff340's xmm0) and the fade after each add, from the start of the process. The breakpoints cost frames: 39 to 41 frames a second in the fight at 60.

| | `main`, 30 | `main`, 60 | branch, 60 |
|---|---|---|---|
| effect step | 1/30 | 1/30 | 1/60 |
| effect seconds per game second (frames × 1/F) in the fight | 1.000 | 2.000 | 1.000 |
| fades: adds to 1 | 30 | 30 | 61 |
| fades: frames from 0 to 1, three owners | 53, 58, 97 | 51, 58, 87 | 113, 120, 242 |
| the same in game seconds | 1.8, 1.9, 3.2 | 0.9, 1.0, 1.5 | 1.9, 2.0, 4.0 |

The fades run while the save loads, where frames come irregularly, so their frame counts vary from owner to owner; `main` at 60 takes half the game time of 30, the branch about the same. The shop's highlight was not run in the game: neither save reaches the shop.

**30 FPS is unchanged.** `rig.py compare` of `main-menu`, the branch at 30 (`114025`) against two runs of `main` (`113902`, `113943`): noise floor 0.0255, ours 0.0248 (ratio 0.97), time ratio 0.97. The branch at 60 (`114107`): ratio 1.44, within twice the floor, time ratio 0.92. No other test load; load average about 2.5 (another session's builds).

## The ragdolls (#567)

**The corpses.** The ragdoll corpses of Central Yharnam are characters whose physics module (chr+0x58) owns a ragdoll object (module +0x20, vtable 0x538d440, built at 0x180c2b0). A watchpoint on a corpse body's centre of mass found the placement: `ragdoll_change_state` (0x1810000), called by the ragdoll update when the requested state (+0xc4) differs from the current one (+0xc0), sets the bodies to the character's pose (state 0, keyframed) and then lets them go (state 2, ragdoll). A probe at 0x1810000 saw every corpse go from −1 to 0 to 2 within a few frames, the corpses one or a few a frame from step 1138 to 1183 of the load at 30; their bodies fall freely while their state is −1. The frame counts of the placement come from that order, one corpse after the other while the street loads, and no timer: at 60 they take half the game time. That happens before the picture of the street settles and is left as it is.

**The step.** `chr_physics_pose_sync` (0x1515ec0) runs once a frame for every character (#556) and hands the ragdoll, the character's transform and a pose buffer to one of three updates, each with a literal 1/30 s (0x4926364) as the step:

| Update | When | What the step does |
|---|---|---|
| `ragdoll_update` (0x180df90) | interval 0, character active | a timer of a radial impulse (+0xf0) counts it down; the bodies of state 0 are keyframed with velocity = distance / step (0xb68a40, hknp's hard keyframe) |
| `ragdoll_update_spaced` (0x1811040) | interval u > 0 | adds it to an accumulator (+0xd0) and drives the ragdoll once the accumulator passes 0.99 · u/30 s (its second argument, u · 1/30), with the accumulated time as the keyframe's time |
| `ragdoll_update_idle` (0x1811650) | not active | adds it to the accumulator and, once that passes min(1.03 s, +0xcc), sets every body's velocity to 0 once |

+0xcc holds the time of the last drive: the step (written by `ragdoll_update` at 0x180e9f9), the spaced update's accumulated time (0x18115a5), or 1/300 s after a state change (0x1810036, 0x18118f6). So a ragdoll that leaves interval 0 is stopped by the idle update after two frames at either rate (a frame count left as it is), and the 1.03 s and 0.99 · u/30 s are upper bounds.

With 1/30 at 60, keyframed bodies got half the velocity they need to reach their pose in a 1/60 step (0xb68a40 divides the distance by the step), and the spaced update drove the ragdoll twice per character update.

**Changed.** `chr_physics_pose_sync` is replaced (`src/game/chr/ragdoll.cpp`): the same calls with `frame_step()` as the step. The spacing u · 1/30 stays: since #573 a character with interval u updates every u 30 FPS frames at any rate. Two per-frame impulses inside `ragdoll_update` would then add up to more at 60, so they are patched (`BB_PATCH`) with a factor `bb_step_ratio` = 30 / F (exactly 1 at 30, 0.5 at 60, set by `set_frame_rate`):

- **The timed radial impulse** (0x180e0af): while the timer runs, every frame each body gets an impulse away from a centre (+0xe0), its strength between +0xfc and +0x100 by distance. With the step at 1/60 the timer lasts the same game time but twice the frames; the patch scales both strengths. Nothing in the check runs started it, and its setter was not found (no store to +0xf0 in the ragdoll's code but the constructor's zero).
- **The anti-penetration pull of the root body** (0x180f15e), behind the debug switch "Enable Penetrate" (0x51282d0, on): every frame it adds (target − position) · 30 to the root's velocity. As a spring that is 900/s² at 30; once a frame at 60 the literal makes it twice as stiff. The patch makes the factor 30 · 30/F. The corpses of the check never took this branch (the same result with it at 15 as at 30).

The other branch of the anti-penetration block sets a body's velocity along a direction to 5 m/s (0x180f2d3, an impulse of (5 − v·d) · mass): a velocity, not an increment, so it stays. State 1 drives the bodies through a controller (+0x90, its virtual +0x28) with control data that 0x1810000 fills with the defaults of Havok's keyframe hierarchy util (hierarchy gain 0.17, velocity gain 0.6, position gain 0.05, ...), gains applied per call. The corpses never enter state 1 (the probe saw only −1, 0 and 2), so it is not changed; whether other characters use it at 60 is open.

**Tests.** `tests/game/ragdoll_test.cpp`. The pose sync against the game's function through the harness, with the four callees faked (they record the step, the second float, the pointers and the flag), in 23 cases: the three paths, the lock object's choice and its one-time init, the active test's branches, the flag at +0x2f4 (also NaN), and fakes that change the module during the init to pin down which fields are read again. At 60 the replacement passes 1/60 to all three and keeps u/30 and the 30.0 and 900.0. A test checks that the replacement and both patches are registered where they belong. The two patches are entered at their place from an assembly driver and captured at the instruction behind them: at 30 the stubs leave the same registers and frame slots as the game's bytes; at 60 they halve the strengths and the pull.

**Check runs.** Scenario `settle` of #553 (Central Yharnam from the `yharnam-route` save, stand 20 s; local), `relwithdebinfo`, RADV, `--window`, with the #553 motion log at five decimals (local probe build: `main` + log, and the branch + log), 2026-11-17 (stamps `20261117-…`, UTC, suffix `-bb-run-settle`): `main` + log at 30 (`020252`) and at 60 (`020423`, `021702`, `024806`), the branch + log at 30 (`024212`) and at 60 (`024341`, `024510`, `024638`). Bodies are matched to the 30 FPS run by their first logged position (within 5 cm: 605 of 652 at 60). Runs of one build at one rate are the noise: they put a body within 0.005 m of where it is in the other runs (median over the bodies; p90 0.1 m at 30 and 0.2 to 0.3 m at 60, the few bodies whose collapse branches).

Per body, from its first move, the time to half and to 90 % of its drop and to its last move, median of (60 − 30) over about 440 bodies with a drop over 5 cm. These are medians of per-body differences; the #553 section above gives differences of the run medians (other runs), which put `main`'s stop 1.4 to 2.0 s early:

| Variant at 60 | half drop | 90 % | stop |
|---|---|---|---|
| `main` (3 runs) | −0.033 s | −0.033 to −0.050 s | −0.94 to −1.23 s |
| branch (3 runs) | −0.017 s | 0.000 s | +0.05 to +0.47 s |

Distance of a body from where the same body is at 30, median over the 605 bodies, at a game time after its first move:

| Variant at 60 | 0.5 s | 1 s | 2 s | 4 s | end (about 20 s) |
|---|---|---|---|---|---|
| `main` (3 runs) | 0.13 to 0.14 m | 0.27 to 0.28 m | 0.29 to 0.33 m | 0.31 to 0.36 m | 0.31 to 0.35 m |
| branch (3 runs) | 0.11 m | 0.18 m | 0.25 to 0.26 m | 0.28 to 0.30 m | 0.31 to 0.34 m |

The drop times at 60 now agree with 30 within a 30 FPS step (median per body). The issue's third measure does not: the bodies stop +0.05 to +0.47 s later than at 30 (median per body, against −0.94 to −1.23 s on `main`). That measure is coarse: its median moves by about 0.4 s between runs of one build, and per body its quartiles are about ±5 s apart, because the last move of a resting body depends on small jitters and on Havok's deactivation. Where the bodies come to rest still differs by about 0.3 m (median) from 30, as much as on `main`.

The cause of that residue is not isolated. Runs with the world stepped by 1/30 every second frame (`BB_HKHALF`, local) and the pose sync's literal at 1/60 (bytes at 0x4926364, which also halves the spacing; `023007`, `023135`) gave the bodies the same median peak speed after their placement as at 30 (2.11 and 2.06 m/s against 2.10, `main` at 60: 3.41), but they still came to rest 0.25 m from 30 and stopped +0.27 and +0.67 s later: with the world at 30's step, the game's per-frame code then runs twice per world step, so neither variant isolates the solver. The collapse is chaotic, so a small difference anywhere grows. Havok's deactivation by step count and the motion properties' per-step limits were not examined.

**30 FPS is unchanged.** At 30 the branch puts every body where `main` does (median 0.000 m, p90 0.02 m, within the noise above). `rig.py compare` of the picture after the stand, the branch at 30 (`025114`, plain build) against two runs of `main` (`024945`, `025243`): noise floor 0.276, ours 0.238 (ratio 0.86), time ratio 1.012. The branch at 60 in the street (`025412`): 60.0 presents a second in the stand, p50 16.67 ms, p99 16.84 ms, no GPU fault. Builds kept the CPU load average at 3 to 4; no other game or test ran meanwhile.

## The parameter blend (#598)

**Fault.** At 60 FPS the game aborted a step after the hunter left Iosefka's Clinic: YEBIS asserted `nBaseSamples >= 3` in `GPUTexUtil_GetRecursiveSampleParameters` (0xbbc3e0), called from its camera motion blur (`GetMotionBlur_CameraRotationPerspectiveParameters`, 0xc32740, call at 0xc33f37). At 30 the same save walks out.

**Cause.** YEBIS takes the motion blur's base sample count every frame from the game (the read at 0x21d7f71 in 0x21d5f80, then 0xbb38f0 → 0xbd3b90, YEBIS context +0x9f4, then +0x2168c of its texture utility through 0xc20770). The game builds it from two motion blur parameter rows blended by 0xe82290, whose ints come from the target row. The rows themselves are eased every frame on the main thread by `param_blend_int` (0x29262f0, through a vtable, from the graphics step at 0x221b8e2): `trunc(entry * t + current * (1 - t))` in single precision, with t = 2 · step. Outside the clinic both values are 3. At 30, t = 1/15 and the sum stays 3. At 60, t = 1/30 and 3 · t + 3 · (1 − t) rounds to 2.9999998, which truncates to 2, and the first frame that blurs with 2 aborts. A watchpoint on the row saw it flip between 2 and 3 from frame to frame; the blend alone can't raise 2 back to 3 (trunc(2 + t) = 2), so something else also rewrites the row, not found. gdb runs at the call: `override 3 current 3 t 0.0333333351 result 2` at 60, only exact results at 30.

**Change.** `param_blend_int` is replaced. At 30 it is the game's function bit for bit (differential test over ids, t including NaN and ±inf, and extreme values). At any other rate an entry equal to the current value gives the current value. Unequal values blend as the game does at both rates: for non-negative values the truncation only lowers a value towards a lower target or holds it below a higher one.

**Check run.** `clinic-door` (a copy of a save at the clinic's door: open it, walk out for 25 s): unpatched 60 FPS aborts in 2 runs of 2, patched 60 FPS walks out in 2 of 2, patched 30 FPS in 1 of 1.

## The 18.7 ms intervals at 60 FPS (#605)

In the check runs of #509 and #514, 1 to 5 % of the present intervals in `play-stand` and `play-walk` were 18.7 ms. That is the vblank period plus `flip_margin` (2 ms): the interval the vblank grid of #528 gives when it follows a flip that came after its vblank. On a quiet machine they don't happen. They need a stall that uses up the flips the game has queued ahead, and those runs had one from other programs.

**Probe.** A local `bb-run` (`main` at 8c01076, not committed) logged on one clock every call of the pacer (when the frame's work ended, when the wait ended) and every vblank of the present thread (when it woke, the flip it took and when that was queued, whether the grid followed, the present call). `clinic-fight` with a warm cache, `relwithdebinfo`, RADV, `--window`, 2026-11-17: five runs at 60 (`162731`, `163256`, `163811`, `164327`, `164842`) and one at 30 (`165357`; stamps `20261117-…`, suffix `-bb-run-clinic-fight`), CPU load average 2 to 3 including the game. The probe did not change the frame times: p50 16.67 ms and p99 16.71 to 16.73 ms in `play-stand`, as without it.

**At 60 on a quiet machine.** Three of the five runs have no present interval above 17.5 ms in any `play-*` segment, and a fourth has one. In them:

- The main loop's work takes 4.4 to 4.9 ms a frame in `play-stand` (p99 5.0 to 5.8 ms) and 3.5 to 3.9 ms in `play-walk` (p99 5.1 to 6.6 ms). The pacer waits about 12 ms of every frame, and no frame goes without a wait.
- The pacer's period is 16,666.6 to 16,666.7 µs in the mean, the grid's. The game does not drift against the grid.
- A flip is queued 1.6 to 2.4 ms after the end of the pacer's wait (p99 up to 4.8 ms).
- A flip waits in the queue for two vblanks: it is queued 32.85 ms before the vblank that takes it (p99 30.4 to 32.6 ms). The grid never follows. At 30 a flip is queued 6 to 10 ms before its vblank.
- The present thread wakes 0.06 ms after its vblank (p99 0.06 to 0.12 ms), and a present call takes 0.14 ms.

**The run with long intervals.** One run (`164842`) has 50 intervals above 17.5 ms in `play-stand` and 16 in `play-walk`, in bursts within nine separate seconds. In those bursts 5.7 % of the main loop's frames in `play-stand` took longer than their slot (up to 48.8 ms; no other run has one but the single frame below), and the delay from the pacer to the queued flip rose to 22.5 ms at p99. Each long interval has a measured cause: for 31 of the 66 flips that started one the main loop's frame was over its slot, for 20 the main loop was on time and the flip was queued late, for 12 both the main loop and the present thread were late, for 3 the present thread alone. The late frames use up the two queued flips, the next flip comes after its vblank, and the grid follows: an interval of 18.7 ms. An interval of 33 to 36 ms is a vblank without a flip. The main thread and the GPU thread slow down together in the bursts, which fits a stall of the whole process, from outside or inside; the logs don't tell which. A browser process started during that run. The one long interval of the fourth run (`163811`) is a present call of 62.0 ms at the moment a main loop frame took 63.4 ms.

**At 30.** `play-stand` has no interval above 35 ms and `play-walk` two (35.4 ms, grid follows with the main loop at 3.5 to 4.4 ms of work). `play-camera` has 26 and `play-lockon` 13, most of them with the main loop on time and the flip queued late: at 30 the flips come 6 to 10 ms before their vblank, the delay from the pacer to the queued flip is 10 to 22 ms, and a flip that crosses its vblank makes the grid follow.

**What follows.** The grid and its margin need no change for 60. Two things are left. At 60 a flip is shown two vblanks, 33 ms, after it is queued, where at 30 it waits 6 to 10 ms: latency that the game at 60 could do without (#606). And the pacer's `late` flag is always 0 in modes 2 and 4, so it does not mark late frames; the frame's work against its slot does.

## Open questions

- Ints that `param_blend_int` eases upward only rise while (target − current) · t ≥ 1, so they stop short of a higher target by fewer than 2/t: up to 15 at 30 and 30 at 60. Such a parameter can settle at a different value at 60 than at 30 (for example 0 → 20 settles at 6 at 30 and stays 0 at 60). Not seen to matter yet (#598).

- Mode 2's `tearing` = 1: where `graphics +0x104`, 0x513ac84 and the render list's byte at +8 end up (presumably the hsync flip when late).
- Whether the Havok behaviour and cloth updates take the task data's step.
- Whether the step-machine functions with task data of 1/30 read `delta_time`.
- `skip_wait` (+0x273, set by 0x1cf4c70 with `SprjScaleform`) and `request_0x274` (sets +0x4c, +0x4d and 60 at +0x50 of the object at 0x5540e00): what state skips the wait.
- The remaining uses of 1/30 and 30.0f that the survey did not classify.
- 0x2277780's step (0x227784b, 0x2277874): what sets the effect system's +0x2977, and whether 0xebbda0 then runs once a frame.
