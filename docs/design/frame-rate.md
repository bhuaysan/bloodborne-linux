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
| 0x143e2b2 | `chase_cam_update` 0x143ac60 | the player's chase camera eases its follow weight `+0x198 += (+0x19c - +0x198) / 30` per call (see "The chase camera" below) | one of the camera's per-call smoothings |
| 0x173456e, 0x15dc1f7, 0x1044943, 0x1f77d4a, 0x227784b, 0x18bc4f7 | the shop's tab highlight (`Ins_tab_select`), the effect system, and four that run once or not at all (#556) | an `update(1/30)` call with a literal | see "Fades, effects and menus (#556)" |

**Task data with a literal 1/30 that probably needs no change** (not verified that the callees ignore `delta_time`): the event system runs a new event once at creation (`SprjEmkSystem`, 0x12efd00); a menu task (0x1b96b00); teardown (0xf73330); and about 30 small step-machine functions (0x1aa3fc0–0x1aa6080, 0x1ab5390–0x1ac3ef0, 0x1b03210–0x1c12bf0) that enter a state through a virtual call with task data of 1/30, most referencing `FrpgNetMan` (the online flow).

**Leave unchanged:** the many `(float)frames * 1/30` conversions (for example 0x17b55ed, 0x17c8754 in the event system, 0xf1e607 and its neighbours, 0x1dbc308 in the AI): they turn frame numbers from the game's data, authored at 30 frames a second, into seconds, and stay right at any frame rate. The 30.0f constants sampled are mostly limits of debug sliders, angles and network values.

**Not resolved:** the Havok behaviour (animation) update and where its time step comes from (`physicsDeltaTime` 0x479e0ec, `hkbGetTimestep` 0x47d5e84); the functor bound to `HavokWorldUpdate_Post` (0x17f9ce0 is the likely one and passes 1/30 in xmm0 from 0x4927554 to 0xfde7f0, whether it is read is unknown); what the menus, effects (`SfxMan`) and the cloth update take as their step.

A first check without tracked code (#509) wrote the flip mode fallback and the tick's step into the eboot after loading. The flip mode alone took the game from 30 to 60 presents a second, in the menus and in play, and the game ran at twice its speed; with the step at 1/60 as well it ran at normal speed. That settled the two changes below.

## The option (#514)

`bb-run --frame-rate 30|60` (default 30) sets the target frame rate F; `rig.py run` and `rig.py perf` pass it on (`--frame-rate`, to `bb-run` only). Two game functions are replaced, in `src/game/frame/`, and are hooked at every frame rate:

1. **`flipper_wait_frame` (0x2034770, 0x84c bytes by its FDE).** The same logic, with `bb::game::frame_step()` = 1/F as the frame time of modes 3 and 4 (the default and the online one) and of the forced default mode. Modes 0 and 1 keep 1/30 (their present interval of 2 caps them at 30 anyway) and mode 2 keeps its 1/60 and its `tearing` = 1, so the game's own 60 FPS mode is not what the option selects. The foreground wait sleeps first (#656): `clock_nanosleep` on `CLOCK_REALTIME`, the clock `gettimeofday` reads, until 2 ms before the end the game computes, then it spins on `gettimeofday` as the game does. The wait still ends with the first read past the frame time, so the frame times stay those of the busy wait, and the main thread no longer burns a core while it waits. With a test clock installed the sleep returns at once, so the differential tests see the game's reads. The 2 ms margin was measured in #656: `clinic-fight` and `yharnam-route` at 30 and 60, three runs each, kept the p95 and p99 of every segment within the spread of the busy wait, and `game main` spends its work plus the margin per frame, about 4 to 6 ms instead of the whole frame.
2. **`main_loop_tick` (0x2018d20, 0x1e0 bytes by its FDE).** The same calls, with `delta_time` = 1/F in the task data it hands to the task manager, so every task and the global task data (0x54b7e08) get the step.

At F = 30, 1/F is the game's own 0x3d088889, and the replacements do what the originals do: their differential tests (`tests/game/frame_test.cpp`) run the original and the replacement from the same state and compare every byte they touch. The pacer reads the clock, so the tests give `gettimeofday` a fake wall clock (`bb::hle::kernel::set_wall_clock`) that moves on by a fixed step with every read; the original and the replacement then wait the same number of reads. The singletons are fakes in the harness's arena whose virtual functions are ours, and the task manager's frame runs for real down to a fake task line that records the task data it gets. Not covered by the tests: the pacer's background branch (`window_is_foreground`, 0xfe3980, always returns true), the creation of the pacer on the first tick (it needs the game's configuration and allocator), the `fd4_singleton_missing` reports (0x20b55b0) and the system object's creation (0x20e2550); these are for the disassembly review.

Replacing the pacer moved `bb-run`'s main-loop probe: it counted the pacer's `gettimeofday` calls by their return address, which now lies in our code. The pacer reports each call to `bb::game::set_frame_observer`, and the probe counts 60 calls.

**Hooks for the other sites.** The 1/30 of the remaining sites is an immediate in code (`mov [rbp-0x48], 0x3d088889`) or a load from a constant next to the function. Writing those bytes in place would bypass the differential harness and the disassembly review, so each site is a replaced function, a wrapper (`BB_WRAP`) or, since #573, a patch of a few instructions with a stub (`BB_PATCH`), each with its own test. Two sites need nothing:

- **`HavokWorldUpdate_Post` (0x17f9ce0).** It loads 1/30 into `xmm0` (0x17f9d26, from 0x4927554, read by nothing else) and tail-calls 0xfde7f0, which never reads `xmm0`: it zeroes it (0xfde83e) before the only call that could take a float. The 1/30 is dead.
- **The three data blocks with 1/30 at the place of `hkp_world_cinfo_init`'s +0x70** (0x2bda86c, 0x2bdc3d4, 0x2bdf7e4) have no references in the analysis.

30 FPS stays the default, and at 30 every replacement in this note does what the game's code does: each has a differential test against the original at 30, and `rig.py compare` of `main-menu` against `main` was within the noise floor for each change.

## What is left at 60 FPS

Each site below was checked at 60 against 30 FPS runs, by pictures or by a local probe build that logged positions:

- **Havok world step** (`havok_world_step`, 0x17f99d0): done in #553.
- **Ragdolls** (`chr_physics_pose_sync`, 0x1515ec0): done in #567. The corpses come to rest about 0.3 m from where they do at 30, from Havok's own sensitivity to the step size; kept (see "The ragdolls").
- **AI step** (`chr_ai_step`, 0x1e2bc10): done in #554.
- **Target bank** (`target_bank_scan_tick`, 0x12a43b0): done in #555.
- **Fades, effects and menus** (`chr_list_fade_step`, 0x180f5e0, and the `update(1/30)` calls): done in #556.
- **Chase camera** (`chase_cam_update`, 0x143ac60): done in #557.
- **AI steering** (`agent_steer`, 0x1dbbd00): done in #570 and #603.
- **Character update intervals** (`world_chr_update`, 0x19173b0) and **setups a frame** (0x18b9890): done in #573. Every agent of the `aiwait` scene walks within 0.2 m of its 30 FPS range in its first 10 s.
- **Parameter blend** (`param_blend_int`, 0x29262f0): done in #598.
- **`chr_pad_step`** (0x1572900) is not the player's input: its 1/30 goes to 0x1d145b0, which turns a matrix by the pad's axes times π/2 and −π/3 rad/s, a debug free camera behind a mode check. Low priority.
- **Input latency**: kept on purpose, see below.

Checks at 60 against 30 hold the rig's inputs for a number of game steps (`frames N`, with N doubled at 60), not for wall-clock seconds: the game never makes up a late frame in modes 2 and 4, so a timed hold covers fewer steps whenever frames miss their slot, and that alone made 60 FPS runs differ from 30 (#558, #562).

## The input latency (#558)

The game reads the pad once a frame and the hunter starts to move two frames after the frame that reads a press, at both rates: 67 ms at 30, 33 ms at 60. The release ends his fast start phase on the frame that reads it, also at both rates. So at 60 a hold of the same game time moves the hunter about 1/30 s longer, 0.13 m on a short side step. Between these transitions both rates move at the same speed.

**Kept.** The latency is the game's own: it counts frames, and a frame is shorter at 60. Adding two frames of input delay at 60 makes the positions match 30 (a probe build checked it), but gives the player 33 ms more input lag, the opposite of what 60 FPS is for. No site is patched. The camera follows the hunter, so the offset shows in pictures of 30 against 60, and it doesn't grow with the length of a walk.

## The chase camera (#557)

The camera is `chase_cam_update` (0x143ac60), called by `cam_mode_update` (0x14368b0, at 0x1436c00 and 0x1436e15) with the step in `xmm0`. Its stick turn, the timers and the wall slide (+0x294) scale with the step. Several filters work per call instead:

| Field | Value (set by `chase_cam_construct` 0x143a0e0) | Use | Per call |
|---|---|---|---|
| +0x198 | 0.1 (imm 0x143a8b7), lerps toward +0x19c (0.1, 0x143a8c1) by 1/30 a call (0x143e2b2, rodata 0x4925e94), reset to +0x1a0 (0.25, 0x143a8cb) | follow point `+0x90 += (target − +0x90) · +0x198` (0x143ba97). The yaw is re-derived from the follow point every call. | yes |
| +0x1ac, +0x1b0, +0x1b4, +0x1b8, +0x1c4 to +0x1cc | 0.1, 0.1, 0.25, 0.03, 0.4, 0.4, 0.3 | lag of the camera position +0xf0 (0x143e2f2 to 0x143e980) | yes |
| +0x240 | 0.3 (0x143aa5b) | yaw and pitch easing while turning on its own (R3 reset, lock-on; 0x143c30f, 0x143c784, 0x143cea0) | yes |
| +0x1a8 | 0.3 (0x143a8df) | pitch pull toward +0x150 (0x143cef0) | yes |
| +0x330 | 0.05 (0x143a7de) | eases the camera parameters (FOV +0x50, +0x160, +0x188, +0x1f0, +0x248) toward the parameter row (0x143ae75 to 0x143af70); debug label "lock camera param interpolation rate" | yes |
| +0x184 | 0.1 (rodata 0x4925e68, one use) | `+0x184 += (+0x188 − +0x184) · 0.1` (0x143af8c), but 0x143bc70 sets +0x184 = +0x188 before its only read (0x143d545): no effect | no |

The follow filter is the one that explains the swing: the follow point catches up with the hunter faster at 60, so the camera turns further during a short walk. A per-call factor k matches 30 FPS at 60 as 1 − (1 − k)^½. For a decomp it becomes 1 − (1 − k)^(30·dt).

Running the whole camera at 30 updates a second brought it to the 30 FPS result, while scaling only the constants above did less, so a fix has to cover every per-call smoothing of the function.

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

## The Havok world step (#553)

`havok_world_step` is replaced (`src/game/havok/world.cpp`): the same calls, with `frame_step()` as the step record's delta time. At 30 it is the game's 1/30.

**The world.** The world the step drives is a world of Havok's newer physics, not an `hkpWorld`: the object the stages get (`[[manager+0x28]+8]`) holds a `hknpGroupCollisionFilter` (its type name in the filter's data), and a motion buffer of 4000 entries of 0x80 bytes at +0xe0 (centre of mass at +0, orientation at +0x10, linear velocity at +0x40; entry 0 is the static motion). The step record is 0x18 bytes: delta time, 4 bytes the game never writes, the manager's +0xc8, the thread pool's thread count + 1, and 0. The manager also checks its job queue's critical section (Havok's `lockMutexWithSpinCount` and `leave`, on `pthread_mutex_*`), runs the thread pool on the queue, calls 0x1e42140 and, after the stages, 0x43b0a0 on the queue. The manager's +0x30 holds the step (it calls 0xb50410 on the world's +0xd8 instead), and +0x26c switches it on. Before the step it hands a per-frame counter of the singleton at 0x553b168 out in units of 200 (0x15a5fc0 per item) and calls 0x17fd220 and 0x1806e90; none of it takes a time.

**Not replaced: `hkp_world_cinfo_init`.** Its +0x70 is the `expectedMinPsiDeltaTime` of an `hkpWorldCinfo` (the defaults around it are Havok's: 200 at +0x68, 250 at +0x6c, solver values 0.6, 1, 4, 1 at +0x88 to +0x94). It configures worlds of the older physics, which this step does not drive. A local `bb-run` that logged its calls and wrapped the creator of one such world (0x10588c0) saw neither run from the boot to standing in Central Yharnam, so changing it would change nothing that we can check. The other stepper that calls the same stages (0xa7a250, slot +0x68 of the vtable 0x5297270) is overridden with an empty function in the only class the game builds from it (vtable 0x53a6b00, made at 0x1e155f9).

**Tests.** `tests/game/havok_test.cpp` runs the original and the replacement from the same state through the harness. It hooks the step's callees (the two stages, 0x43b0a0, 0x1e42140, 0xb50410, 0x17fd220, 0x1806e90 and 0x15a5fc0) with fakes that record their arguments, so the harness compares the step record the stages get, the calls and their order, besides every byte the step touches. The stages themselves are Havok's code and stay the game's in `bb-run`. The mutex calls are our HLE's and are not seen by the tests, so the lock and its failure paths are for the disassembly review.

With the step at 1/60 bodies fall at g in game time at both rates; with the game's 1/30 at 60 they fell at twice g. The corpses are placed by teleporting their bodies after a fixed number of frames, which is outside the world step (see "The ragdolls").

## The AI step (#554)

`chr_ai_step` (0x1e2bc10) runs once a frame from `chr_frame_update` (0x1548740, call at 0x154972c) with the `SprjWorldNvmManager` (0x5540208). Its only other caller is a step of the test module `ModuleNaviMeshTestStep` (0x19baa40, call at 0x19baf55); its call of the AI world step returns to the same address, so it gets `frame_step()` too. Its only use of 1/30 is the task data it builds on its stack (0x1e2bcd1) for one call at its start: `ai_world_step` (0x1dcdad0) on the AI world manager (`SprjHkAiManager` +0x10). That step reads nothing of the task data but `delta_time`, which it passes in `xmm0` to the Havok AI world's step (0x8a0790, a thunk of 0x8a07a0, which takes the manager's +0x78 and +0x80 when +0xf0 is set). The rest of `chr_ai_step` (a loop over the world's +0x2bc8 entries under its third argument; under its fourth, calls with `RendMan` and `WorldChrManDbg` that look like debug drawing) takes no step.

**Wrapped at the call, not decompiled.** `ai_world_step` is wrapped (`src/game/ai/world_step.cpp`, `BB_WRAP`). The hook enters the wrapper with a `jmp`, so its return address is the game's call site: when that is `chr_ai_step`'s call (return address 0x1e2bceb) and the frame rate is not 30, the wrapper hands the original a copy of the task data with `frame_step()`. Every other call keeps its task data: the only other call is in `hkai_test_module_step` (0x19a6860), a step of the test module `ModuleHavokAiTestStep`, which passes on the task data it gets from the task tree. A thunk in padding at 0x1dc91b0 (`mov rdi, [rdi+0x10]; jmp 0x1dcdad0`) has no references; were it used, its tail jump would leave its caller's return address, never 0x1e2bceb. This changes the one value and leaves the 2.8 KB of `chr_ai_step` the game's code. At 30 the wrapper calls the original unchanged.

**Left as it is: the section sweep.** `ai_world_step` hands sections whose bounds changed (flag 4 at +0xc0 of a section object) to the world step in slices of a thirtieth of all sections a call, so one sweep takes 30 calls: 1 s at 30, 0.5 s at 60. That only decides how soon a change is passed on, and at 60 it is sooner.

**Tests.** `tests/game/ai_test.cpp` hooks the Havok AI world's calls in the step (0x8c8360, 0x897390, 0x895bd0 and 0x8a07a0; 0x8c8350 and 0x8a0790 are thunks too short to hook) with fakes that log their arguments and the contents of the step's arrays (they live on its stack, whose address differs with the wrapper's frame). The harness compares the original and the wrapper called directly at 30 and at 60 (both keep the caller's task data), and `chr_ai_step`, run as the game's code up to its call of the step, without and with the registered hook at 30: every byte is the same. At 60, through the hook, the Havok AI world gets 1/60 (bits `0x3c888889`), and without it 1/30.

The enemies of the `aiwait` scene (the `yharnam-fight` save, standing 30 s while they walk up) appear, start and walk at the same game time and speed at 30 and at 60. Five of them still walked less at 60; that was the steering ring (#570, below).

## The target bank (#555)

`target_bank_scan_tick` (0x12a43b0) is a member functor of a per-frame task (built at 0x1a654b8) on the `SprjTargetBankManager` singleton (0x553b098, built by 0x12a3dd0). It does two things per call, neither with the frame's step:

- **The bank.** A timer (+0x2c) goes up by a literal 1/30 (0x12a4416). When it reaches the interval (+0x28, 5 s; the timer starts at 5, so the first call scans), the tick resets it and rebuilds the bank, a vector of characters at +0x08 to +0x18: every character of `WorldChrMan`'s list (+0x30, +0x38) and of the chr set the singleton at 0x553b148 names (+0x20) whose virtual +0x78 says yes. Between rebuilds the bank only loses characters that are destroyed (0x1a554f0, 0x18bc110). It is read by 0x122b3f0 and 0x12adc50. So a character that appears waits for the next rebuild, up to 5 s, before it is in the bank.
- **The AI update schedule** (debug label `SprjAiUpdateTimeSchedular`). A ring of 100 byte counters at +0x30 with its index at +0x94: each call clears the slot at the index and moves the index on by one. The AI's think update (0x12ab4c0) books its next think 1.0 to 1.2 s ahead (a random time) in the slot `seconds * 30` past the index (0x12ab623). A slot holds at most +0x98 (3) bookings; when it is full, the booking moves on to the next free slot and the think time grows by 1/30 s per slot skipped. `Disable System` (+0x99) switches the limit off. The think timer itself counts seconds; the ring only spreads the thinks over the frames.

At 60 the tick ran twice as often per game second: the bank was rebuilt every 2.5 s, and each slot of the ring stood for 1/60 s where the bookings count it as 1/30 s.

**Wrapped, not decompiled.** The hook (`src/game/ai/target_bank.cpp`, `BB_WRAP`) runs the game's tick on every (frame rate / 30)-th call: every call at 30, every second call at 60. The timer, the rebuilds and the ring then move on 30 times a game second, as at 30 FPS, bit for bit the same sequence of values. The index stays on one slot for two frames at 60, which is what the bookings' `seconds * 30` assumes. The bank's readers see it rebuilt on the first frame of a pair, at the same game time as at 30. The phase of the pairs is a counter in our code, arbitrary at the start; the tick takes no task data.

**Tests.** `tests/game/target_bank_test.cpp`: the game's tick through the registered hook at 30 leaves every byte as without it (1 and 160 calls), and at 60 twice as many calls through the hook leave what the game's tick leaves in the same number of calls at 30: the timer, the bank and the ring. Fakes stand in for the world's characters (a vtable whose +0x78 reads a byte) and the two singletons; the bank has room for every character, so it never grows. Four cases: the game's own state at creation, a short interval with the ring wrapping from 99 to 0, and no player chr set (-1 and one past the end). Without the wrapper the 60 FPS calls differ (a test checks that the comparison sees it).

## The steering ring (#570)

**Found by halving.** A local probe build ran one game function per object only every second call at 60, with twice the step, and logged the AI agents' positions. Halving the path update (0x1234750) inside the AI character update removed the shortfall of five enemies that walked 2 to 4 m less in their first 10 s at 60. At 60 the point those agents steer at was their own position once every 30 calls, never at 30, so the path update's stuck checks started over and the agent got no direction for a frame.

**The ring.** The point comes from `agent_steer` (0x1dbbd00), virtual +0x50 of the AI agent (vtable 0x534e220; the agent whose Havok AI character is at +0x230), which the path follower (0x1239eb0) calls with the frame's task data. It stores the step at +0x280, sets +0x28c (the AI world step, 0x1dcde52, reads and clears it), runs a small state machine (+0x258, timer +0x25c) and writes the direction to the next path point into a ring of 30 slots (+0x30, 16 bytes each, write index +0x24c, modulo 30 at 0x1dbc2d4). It returns the character's position plus 3 times the average of the slots from a read index (+0x248) up to the newest, and keeps that direction at +0x20. The read index lags at most `+0x250 × (1/30) / delta_time` calls behind (0x1dbc2fd); +0x250 is a lag in 30 FPS frames that the agent's setter (0x1dbbc69) computes from the speed, 1 to 29, high when the agent walks slowly. At 30 the lag fits the ring. At 60 it is twice as many calls, and from 15 on it reaches the ring's size: the read index meets the write index once a round, the average is empty, and the point is the agent's position. The state machine's state 1 also counts its timer down by a literal 1/30 a call (0x1dbc5cc), and the setter turns the stored step into a look-ahead distance (0x1dbbc8f).

**Wrapped, not decompiled.** `agent_steer` is wrapped (`src/game/ai/steering.cpp`, `BB_WRAP`). At 30 the wrapper calls the original. At 60 it counts each agent's calls and calls the original on every second one, with the task data's step doubled (2 × 1/60 is the game's 1/30 bit for bit), so the ring, the lag, the timer and the stored step move on as often a game second as at 30. On the other calls an agent with a Havok AI character gets two of the steering's writes on every call (+0x28c = 1, the character's +0x64 to +0x1c of the object at `[[agent+0x238]+0x58]`) and the point the original returns, built the same way from the character's position now and the direction at +0x20 (0x1dbc4d2 to 0x1dbc529: multiply, then add, no fused multiply-add). The third write, clearing the requests at +0x289 and +0x28a (0x1dbc52c), is left out: only the steering acts on them (set by 0x1dbb990, read at 0x1dbc69a), so they wait for the next call that runs it. An agent without a character gets the original, which returns a constant, and its count starts over, so its first call with a character runs the original.

The count is per agent, not per frame: the path follower doesn't step every agent every frame. A log of the steering at 60 showed agents called on every frame, on every second and on every fifth. A first version that ran the original on even frames left agents stepped every second frame on odd frames only: one ran the steering 16 times in 919 calls, five enemies of the scene stood still and one ran at the hunter. The counts live in a table by the agent's address under a lock (the agents are stepped on several `CSChrThread`s); the table starts over when it holds 4096 agents, which costs each agent at most one extra run.

**Only agents of characters with interval 0 (#603).** Since #573 a character with an update interval u > 0 is updated every 2u frames at 60 with a step of 2u/60 s, as often a game second as at 30 and with the same step. `agent_steer` is reached only from inside that update (0x18bdb20, then `chr_ai_control_update`, 0x1513c10), so halving it too ran these agents at half their rate at 30. The step in the task data can't tell the two cases apart (0x1a38430 scales it in hit-stop), so `chr_ai_control_update` is wrapped as well: it notes the character's interval for the thread while it runs (its second argument is the character's control object, the character is the pointer at +8 of it, the interval the int32 at +0xe4 of the character), and `agent_steer` halves only when no character is noted (a call from elsewhere) or the interval is 0. Otherwise it calls the original with the caller's task data and sets the agent's count to 0. The note is a `thread_local` saved and restored around the call, because the game steps an agent on the thread that updates its character. At 30 the wrapper only calls the original.

**A fresh agent runs at once.** The counts are kept by the agent's address, so a new agent at the address of a destroyed one inherits its count and may skip its first call, which then returns its own position plus a direction the steering never computed. The agent's constructor (0x1dbae40, store at 0x1dbaf4b) sets the stored step (+0x280) to 0.0f and only `agent_steer` writes it (0x1dbbd45), so a step of 0.0f means the steering has not run on the agent: the wrapper runs it now whatever the count says, and the count goes on from there, so the next call is the skipped one. But a run with a step of exactly 0 stores 0.0f too, and in a full hit-stop (0x1a38430) the step is 0 for several frames, so 0.0f alone would make the wrapper run the original on every call. The wrapper therefore keeps, next to the count, whether the last run it passed on had a step of 0, and a 0.0f then counts as fresh only if that was not so. A new agent at an address whose previous agent last ran with step 0 just follows the inherited count.

**Tests.** `tests/game/steering_test.cpp` runs the game's steering on an agent of the game's class whose character is in state 3, so the steering takes the direction to the next point of a path (the game's 0x1dbca40 and 0x89c030 on a path in the arena), with lags of 5, 20 and 29 frames, straight and bending. Through the registered hook at 30 every byte and every returned point is the game's (120 calls). At 60, 240 calls through the hook leave what 120 calls of the game's steering leave at 30: the ring, the state, the step, and the points, the second call of each pair returning the point of the first. Without the wrapper the game's steering at 60 returns the agent's own position once every 30 calls with a lag of 20, at 30 never, and through the hook at 60 never. Tests of `agent_steer_every` with a stand-in check the call pattern, an agent without a character, and the point and writes of a skipped call.

## The character update interval (#573)

**Measure with enough digits.** An AI log printed with four significant digits made two differences between the rates that weren't there: positions resolved to 0.1 m, and agents were matched by a rounded start position. Probes now log seven digits and match agents by the nearest start position.

**The patrol stops.** What was left is real: the two enemies that patrol between two points (starting at (−192, 83) and (−215, 104)) stop at each point for 1.97 s at 30 and for 1.57 s at 60, every cycle. At the point they stop, turn on the spot and walk on. The stop and the turn begin and end at the same game time at both rates. At 30 they walk on 0.3 to 0.5 s after the turn, at 60 just before it ends. Halving `chr_frame_update`, the AI character update (0x128f060), the manipulator update or the AI agent update per object (as in #570) left the stop at 1.57 s. A watchpoint on the Havok AI character's facing found its writer, the agent's virtual +0x40 (0x1dbba90), called from 0x12355c0 on the character threads. The facing changes only every fifth frame, at both rates. At 30 the stop is 1.97 s when the agent's facing is updated every fifth frame and 1.50 s when it is updated every second frame.

**The interval.** `world_chr_update` (0x19173b0) runs once a frame from WorldChrMan's frame update (0x1915320, which counts the frames at WorldChrMan +0xa68). It lists the characters due this frame and runs their updates, most of them as jobs on the `CSChrThread`s. Each character has an update interval in frames at +0xe4, set by the frame update from the character's distance: 0 is every frame, and with u > 0 the character is due when its index in its group modulo u equals the frame counter modulo u (0x1917909), with a step of u frames (`vcvtsi2ss` and `vmulss` at 0x1917923). Its own time is right at any frame rate: the elapsed time reaches it as the step (+0xe0). But the number of updates a game second is 30/u at 30 and 60/u at 60. The patrol stop takes a fixed number of a character's updates (about three more than the turn), so at 60 it was shorter. The interval is also a level of detail that the characters' own code tests against fixed values (−1, 0, 2, 5, 30, and a switch over −1 to 5 at 0x18cd1fa), and that `world_chr_update` keeps at [rbp−0xc8] for its counts, so doubling the field itself would change what the characters do.

**Patched inside the function.** The hook framework can now replace instructions inside a function (`BB_PATCH`, `docs/design/runtime.md`, "Hooks"). The 36 bytes at 0x1917909 jump to a stub (`src/game/chr/update_interval.cpp`) that does what they do with k·u in place of u in the two divisions and the step's factor, where k is the frame rate / 30, and keeps u in ecx and [rbp−0xc8]. At 60 a character with interval u is then due every 2u frames, with 2u steps of 1/60: as often a game second as at 30, with the same step. Characters with interval 0 still update every frame. `world_chr_update` is wrapped (`BB_WRAP`) to set k before the game's code runs; `hook::apply` fills the stub's two jump targets (#651). At 30, k is 1 and the stub does what the game's bytes do.

**Tests.** `tests/game/chr_update_interval_test.cpp` enters the game's code at 0x1917909 from an assembly driver with set registers and frame, and two patches at the instructions behind the check (0x191792d, due; 0x1917a1b, not due) record the registers and the frame slot. For intervals 1, 2, 3, 5 and 30, indexes 0 to 39 and frame counters up to 2³²−1: at 30 the stub leaves exactly what the game's bytes leave; at 60 it leaves what the game's bytes leave for 2u, except that ecx and [rbp−0xc8] stay u. Over 6u frames at 60 every character is due 3 times with 2u steps, where the game's bytes are due 6 times. The wrapper sets k from the frame rate. `tests/hook/hook_test.cpp` covers `BB_PATCH`: the patch, its refusals (the function is not an FDE start, the bytes do not lie inside it, differ from memory, are fewer than 14 or more than 64; no continuation; a jump target without a slot, outside the function or inside the patched bytes) and overlaps with other entries and live hooks, and that `apply` fills the continuation and the jump targets.

**Characters set up after the load.** The group of enemies that is there when the save loads appeared in the AI world 0.2 to 0.4 s earlier at 60, counted from the first agent: a fixed number of frames after it at both rates. A breakpoint on the AI agent's constructor (0x1dbae40) found one agent built per frame, from a character's setup gate (0x18b9890) in its state step (0x18bd320), which WorldChrMan's frame round `chr_frame_update` (0x1548740) runs for every character once a frame. The round hands each step a budget {limit, count} with the limit from the parameter `WorldChrMan.MaxValidateNum` (WorldChrMan +0x5f90, 1 by default), and the gate sets a character up only while the count is below the limit, except characters whose virtual +0x1b8 says so. So the game sets up one character a frame, and at 60 the group was complete in half the game time.

**Paced per 30 FPS frame of the world.** The gate is wrapped (`BB_WRAP`, `src/game/chr/validate_pace.cpp`): at 60, on frames whose WorldChrMan counter (+0xa68) is odd, it hands the game's gate a budget with limit 0 and the caller's count, and copies the count back. The limit then holds per two frames, as often a game second as at 30; the characters the budget skips, a budget without limit (−1) and every other wait of the gate stay as they are. The counter decides only while it advances. Only WorldChrMan's frame update (0x1915320) adds to it, after the round in the same step (0x193ac10); the load steps (0x1939bb0, 0x1939e90, 0x193a2d0, 0x193a3e0) and one mode of the step run the round without it. In those frames no game time passes, and pacing them by our main loop's frame count instead made the group appear 0.1 to 0.2 s late at 60: fewer characters were set up by the time the world ran. Pacing by a counter that stands still could also stop all setups on an odd value. Each round of a frame in which the world goes on sees a new value (the round comes before the update, so it sees the value of the previous frame's update). So when the counter has not changed in this main loop frame (`frame_count()`, one per `main_loop_tick`), the gate runs as the game's, once a frame; that includes the first frame after a stand-still. At 30 the wrapper calls the game's gate with the caller's budget.

**Tests.** `tests/game/chr_validate_pace_test.cpp` runs the game's gate with a fake character (a vtable whose +0x1b8 and +0x60 are fakes) and fakes of the singletons it checks. Through the registered hook at 30 it does what it does without the hook, for budgets with and without limit, spent and not, on any frame. At 60, with the counter advancing, it does the same on even frames, and on odd ones what the game's gate does with limit 0. The gate's other waits (a null character, a behaviour slot in state 1 or 2, the object at +0x288) are the same through the hook at both rates and parities. Ten waiting characters offered a budget of 1 each frame are set up in 10 frames at 30 and in 20 at 60. While the counter stands still on an odd value the gate sets up a character every frame, as the game's. `WorldFrames`, which tells the world's frames, is tested on its own, and `tests/game/frame_test.cpp` checks that a tick adds 1 to `frame_count()`.

**Check runs (`aiwait`, AI log with 7 digits).** With the wrapper every agent of the load group starts walking at the same game time as at 30 (it was 0.2 to 0.4 s earlier), and (−230.9, 45.6) walks 8.1 to 8.2 m in its first 10 s at 60, 8.0 to 8.1 m at 30 (7.7 to 7.8 m at 60 before). In the two runs of the final version every agent's 10 s distance at 60 is within 0.2 m of the range of five runs at 30. At 30 the runs with the wrapper differ from runs of `main` no more than two runs of `main` differ from each other.

**The agent in contact.** (−231.8, 43.3) walks the same path at both rates until other enemies of a later group come up next to it, about 5.5 s after it starts. From there it passes (−234.0, 38.2) on one of two sides, and the two paths differ by 0.8 m after 10 s. Which side it takes follows from when (−232.4, 40.1) starts moving, counted from its own start. Across 20 runs, it took the east side whenever that offset was at most 142.5 frames at 30 FPS (4.75 s) and the west side whenever it was 143.5 or more. There is no push between the characters that differs per frame: up to the split, the tracks of both branches agree. At 30 the offset is 140 to 141 frames, and every run takes the east side. With the final pacing at 60 it is 142.5 in every run and every run takes the east side. The first wrapper, paced by the raw counter, gave 143.5 in 2 of 4 runs, and pacing by the main loop's count gave 146.5 or more. In both cases the agent took the west side and walked 10.8 m in 10 s. The 2.5 frames come from the setup order after the load (the agent comes 1.5 frames earlier counted from the first agent, (−232.4, 40.1) one frame later). In six runs of the final version at 60, every agent's 10 s distance is within 0.16 m of its range in nine runs at 30 (the agent in contact: 9.8 to 10.1 m at 60, 9.9 to 10.4 m at 30).

## Fades, effects and menus (#556)

**What the sites are.** A gdb probe counted the hits of each literal 1/30 site per frame at 30, at the title, in the pause menu and inventory and in `yharnam-fight`:

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

**Patched inside the functions.** Each site is a `BB_PATCH` of the few instructions around the literal (`src/game/frame/step_literals.cpp`): the stub does what those bytes do with `bb_frame_step` (1/F, written by `set_frame_rate`) in place of the literal, and jumps back. At 30, `bb_frame_step` is the game's 0x3d088889. `hook::apply` fills the stubs' jump targets when it writes the patches (#651). The effect patch takes the skip test with it (0x1f77d3b to 0x1f77d52), because the literal's load is the target of no jump but the call behind it is.

**Tests.** `tests/game/step_literals_test.cpp` runs each of the three game functions whole, with fakes for its callees, once with the game's bytes and once with the patch, from the same state: at 30 the harness finds no difference in anything they touch (effects with and without the skip flag; the shop with and without the instance, in both tab modes; the fade at 0, 0.5, 0.98, 0.99, 1, 1.5 and −0.25 with 0, 1 and 4 parts). At 60 the effect system and the highlight get 1/60 (the skip still 0), and the fade reaches 1 in 60 calls where the game's bytes take 30. With 1/60 in place of 1/30 at 30 every comparison fails.

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

**Tests.** `tests/game/ragdoll_test.cpp` runs the pose sync against the game's function through the harness, with the four callees faked, over the three paths, the lock object's init, the active test's branches and fakes that change the module during the init. At 60 the replacement passes 1/60 to all three and keeps u/30 and the 30.0 and 900.0. The two patches are entered at their place from an assembly driver: at 30 the stubs leave the same registers and frame slots as the game's bytes; at 60 they halve the strengths and the pull.

**Left: Havok's step size.** The bodies' drop times agree with 30 within a 30 FPS step. At 60 they still come to rest about 0.35 m (median per body) from where they do at 30, and a body settles (stays within 5 cm of its rest position) a median 0.4 s later, counted from its first move, with ±1 s per body. That is not noise: two runs of one build at 60 differ by 0.003 m and 0.00 s (quartiles ±0.05 s). The probe's "last move" (the last step faster than 1 cm/s) is noise, though: it differs by 0.4 to 0.7 s per body between runs of one build, and that was the 0.4 s run-to-run wobble of the stop time.

The cause is Havok itself. A local probe ran the game at 30 and stepped only the Havok world twice a frame at 1/60, so every game function ran as at 30. The corpses then came to rest 0.32 m from 30 and settled a median 0.3 s *earlier* (±1 s per body), the same size as at 60 with the other sign. Four steps a frame at 1/120 did not converge either: every step size (1/30, 1/60, 1/120, and 60 FPS) rests 0.40 to 0.53 m from every other. The collapse of a pile of ragdolls is chaotic, so any change of the step gives other rest positions and settle times, and there is no rest position at 30 for 60 to match. Medians were bootstrapped over the 47 to 52 corpses (bodies grouped by position).

The rest of the shift at 60 comes from the corpse placement at load (above): at 60 it runs in half the game time, so the bodies first move about 2.2 s of game time earlier than at 30 and settle about 1.8 s earlier in game time. At both rates every corpse has settled more than 3 s before the hunter stands in the street (`settle`'s standing mark), so none of this shows in play. The issue's stop-time check (within one 30 FPS step) can't be met by any change of the physics step and was dropped; matching 30 exactly would need the world stepped at 1/30 with interpolated poses at 60. Not examined: a death fling (the two patched impulses in a game run) and state 1.

## The parameter blend (#598)

**Fault.** At 60 FPS the game aborted a step after the hunter left Iosefka's Clinic: YEBIS asserted `nBaseSamples >= 3` in `GPUTexUtil_GetRecursiveSampleParameters` (0xbbc3e0), called from its camera motion blur (`GetMotionBlur_CameraRotationPerspectiveParameters`, 0xc32740, call at 0xc33f37). At 30 the same save walks out.

**Cause.** YEBIS takes the motion blur's base sample count every frame from the game (the read at 0x21d7f71 in 0x21d5f80, then 0xbb38f0 → 0xbd3b90, YEBIS context +0x9f4, then +0x2168c of its texture utility through 0xc20770). The game builds it from two motion blur parameter rows blended by 0xe82290, whose ints come from the target row. The rows themselves are eased every frame on the main thread by `param_blend_int` (0x29262f0, through a vtable, from the graphics step at 0x221b8e2): `trunc(entry * t + current * (1 - t))` in single precision, with t = 2 · step. Outside the clinic both values are 3. At 30, t = 1/15 and the sum stays 3. At 60, t = 1/30 and 3 · t + 3 · (1 − t) rounds to 2.9999998, which truncates to 2, and the first frame that blurs with 2 aborts. A watchpoint on the row saw it flip between 2 and 3 from frame to frame; the blend alone can't raise 2 back to 3 (trunc(2 + t) = 2), so something else also rewrites the row, not found. gdb runs at the call: `override 3 current 3 t 0.0333333351 result 2` at 60, only exact results at 30.

**Change.** `param_blend_int` is replaced. At 30 it is the game's function bit for bit (differential test over ids, t including NaN and ±inf, and extreme values). At any other rate an entry equal to the current value gives the current value. Unequal values blend as the game does at both rates: for non-negative values the truncation only lowers a value towards a lower target or holds it below a higher one.

The scenario `clinic-door.scn` checks it: unpatched 60 FPS aborted when the hunter left the clinic, patched it walks out.

## The flip queue at 60 FPS (#605, #606, #608)

**18.7 ms intervals.** At 60, 1 to 5 % of the present intervals were 18.7 ms in some runs: the vblank period plus `flip_margin` (2 ms), the interval the vblank grid of #528 gives when it follows a flip that came after its vblank. On a quiet machine they don't happen; they need a stall that uses up the flips the game has queued ahead, and those runs had one from other programs. The grid and its margin need no change for 60 (#605).

**The backlog.** At 60 a flip waited in video out's queue for two vblanks (33 ms), where at 30 it waits 6 to 10 ms (#606). A stall in the title sequence leaves vblanks without a flip, and then the GPU thread queues several flips at once. The game sets flip rate 0, so the present thread takes one flip per vblank, and at 60 the game supplies one flip per vblank: a depth of 2 never drains. At 30 the same burst drains within five vblanks, because the game supplies a flip only every second vblank.

**Decision.** A vblank that finds more than one flip queued shows the newest and completes the older ones without presenting them. "Queued" means in the queue at the moment the present thread receives a request. A flip that arrives later, also within the grace after the vblank, waits for the next vblank. A skipped flip completes exactly as a shown one does for the game, in queue order and before the newest: flip status, flip events and the label of the buffer shown before. Only the presentation is left out, and its frame goes back to the presenter's free queue once a small GPU submit, which waits for the frame's draw work and signals its present fence, covers that work (a resize can then recreate the frame safely). The vblank grid follows the newest flip, the one that is shown. Every older flip was queued before it, so a late older flip means a late newest one. Without a presenter, or with one that shows nothing (`bb-run --headless` has a presenter but no window, so its frames are null), nothing is shown, so flips keep completing one per vblank, the pace the game gets from them (`FlipPresenter::shows_frames` decides, not each request's frame). With one flip queued, or none, nothing changes.

**Reason.** The two flips of a backlog are the game's last two frames. Showing the older one only adds a vblank of latency to a frame that the next one replaces, and it keeps the queue at 2 for good. Dropping the older one brings the wait of a flip at 60 below one vblank period and keeps its place in the sequence for the game, which counts completed flips.

**Cost.** A frame that was queued behind a stall is never shown. The player loses it only when a backlog exists; at most one frame per vblank was shown before and is shown now.

The backlog was also a buffer of one frame, and it is gone. The game's flips reach the queue 5 to 14 ms before their vblank, and that phase wanders by tenths of a millisecond per frame while the pacer keeps its period. When a flip slips past the vblank, the grid follows it, which gives one interval of about 18.7 ms. With the backlog the flip of the frame before was shown at that vblank.

## Slow phases at 60 FPS (#610)

While #608 was being checked, two runs of `clinic-fight` at 60 had a phase of 20 to 60 s in which about half the frames missed their slot. In those phases the GPU thread used 88 to 97 % of a core, against 47 to 75 % outside them. The game's render thread waited in `sceGnmSubmitCommandBuffers`, and the main loop waited for the render thread.

**Cause: load from other programs.** The GPU thread does the same work in a phase. The other programs make that work run slower and wait for a core. No function of ours or of the driver takes the extra time on its own. Measured on `main` after #592 and #593 (runs with `--perf`, schedstat of the GPU thread every 250 ms):

| `clinic-fight` at 60 | runs | GPU thread, share of a core | run-queue wait | presents over 17.5 ms |
|---|---|---|---|---|
| quiet machine (about 1 core busy outside the game) | 3 | 0.65 standing, 0.32 to 0.48 moving | under 1 % | 0 to 5 of 300 per 5 s, no phase |
| 6 busy loops for 60 s (6 to 9.5 cores outside) | 1 | 0.40 to 0.59 | up to 2 % | 0 to 37 of 300 per 5 s, scattered |
| 12 busy loops for 60 s (13.5 cores outside) | 1 | 0.58 to 0.63 | 9 to 12 % | 35 to 70 of 290 per 5 s, for exactly the 60 s |

The core clock stayed at 3.9 GHz throughout. In `play-dodge`, perf counts 5.55 ms of GPU-thread CPU per present on a quiet machine (5.57 in a second quiet run) and 10.07 ms under 12 busy loops. The rise is spread over every function: the rasterizer's bind calls, RADV's state emission and the PM4 parser all grow, from about 20 % (`memmove`) to several times for small RADV functions. The biggest single rise is 0.17 ms (`Rasterizer::BindBuffers`), and between the two quiet runs no function differs by more than 0.03 ms. The machine has 8 cores with two threads each, so another process on the GPU thread's sibling slows it down although the thread still gets its core.

**Why the phases saturated a core before.** Before #592 and #593 the GPU thread needed 47 to 75 % of a core at 60. A slowdown of 1.7 to 1.8 times takes that to a full core and more, which is the 88 to 97 % of the #608 runs. On current `main` the same load leaves the GPU thread at about 60 % of a core, and frames miss their slot through run-queue waits instead. No phase appeared in the three quiet runs.

**Decision.** No change for #610: there is no cost that rises only in the phases. Frame-time measurements still need a quiet machine (`docs/test-rig.md`, "Thread times"). Making the GPU thread cheaper per draw, as #592 and #593 did, is also what keeps load from other programs from saturating its core. One phase in #608 came without a rise in the outside load that the probe saw. The probe measured only the game's threads, so what ran next to the game then is not known.

## The final check (#514)

After every site above had landed, the check of #514 ran on `main` at d5c8f6f, on a quiet machine (load 2.5 to 4.6 from the desktop, no compiler running, build lock held for each run). Every run counted ended in `end,scenario finished`.

**Frame times at 60.** `rig.py perf clinic-fight --runs 3 --frame-rate 60`: the median present interval is 16.67 ms in every segment, from the logos to `play-walk-again`, with p95 16.70 to 16.72 ms and p99 at most 18.74 ms. The same three runs at 30 hold 33.33 ms in every segment.

**Durations at 30 and 60.** The rig checks a picture at most once a second, so the durations come from game state. A local probe build (not committed) logged the hunter's position (`[chr+0x58]+0x350`) once a frame from the chase camera and the time of every pad read, and the rig's Vulkan layer wrote a 64×36 picture of every present during the menu part. Time is counted in game steps: pad reads (one per frame) times 1/F. A scenario held its inputs for frames, N at 30 and 2N at 60: four walks of 2 s, four back steps, four side dodges, four punches, two charged attacks, and the pause menu opened and closed three times. Three runs at each rate, after one run to warm the cache. A fourth run at 30 ended in the GXWorker segfault at 0x263b8e7 (#559), at 30 FPS; it was left out.

The noise first. The three 30 FPS runs agree to the step on every event but two: the first punch (end of motion 1167 to 1333 ms after the press) and two settles after a dodge (33 ms). The 60 FPS runs agree within one 60 FPS step on all events that don't touch a wall.

| What | 30 FPS | 60 FPS |
|---|---|---|
| walking speed, 0.5 to 1.5 s into a hold | 4.0006 m/s | 4.0006 to 4.0007 m/s |
| distance of a 2 s walk | 7.09 m | 7.22 m |
| back step: press to end of motion, distance | 1333 ms, 2.30 m | 1300 to 1317 ms, 2.26 m |
| free side dodge: end of motion, distance | 600 ms, 3.08 m | 583 ms, 2.99 to 3.07 m |
| punch (2nd to 4th): end of motion | 1267 ms | 1233 ms |
| charged attack: end of motion, distance | 1400 ms, 0.30 to 0.31 m | 1400 ms, 0.32 m |
| pause menu opens: first to last changed picture | 167 ms | 167 ms |
| pause menu closes | 133 ms | 167 ms |

Every duration matches within one 30 FPS frame, most within one 60 FPS frame. Motion starts one 60 FPS frame earlier at 60, the input latency of two frames that is kept (see "The input latency"), and that also gives the walk its extra 0.13 m. The menu pictures change only every second step at 60, so the menu animates at 30 Hz at both rates, and its 33 ms at 30 is the resolution of the 30 FPS run.

Some events differ in distance by more than this: the dodges and punches in the corner by the clinic door, where the hunter meets a wall. The four back steps end at the wall 0.09 m further at 60, since the walks before them started from a place 0.15 m apart, and the side dodges after them then hit the wall at other places (2.17 m at 30, 2.62 m at 60, and in one 60 FPS run 0.95 m). One 60 FPS run ended up 1.5 m from the others and its punches slid along the wall for up to 2 s. Where the hunter does not touch the wall, the same dodge covers 3.08 m at both rates. The difference is where the hunter stands, not how long things take.

**Nothing changes at 30.** `rig.py compare` of `main-menu` at 30, with two runs of a `bb-run` built before the 60 FPS work (e2f1cf6^) as the noise floor and current `main` as ours: ratio 1.029 and 1.018 for two runs, time to the menu 19.83 and 20.54 s against 19.81 and 19.82 s (`time_ok`). Against two shadPS4 reference runs, current `main` gives 1.045 and 1.032 and the old build 0.997 and 1.007.

## Open questions

- Ints that `param_blend_int` eases upward only rise while (target − current) · t ≥ 1, so they stop short of a higher target by fewer than 2/t: up to 15 at 30 and 30 at 60. Such a parameter can settle at a different value at 60 than at 30 (for example 0 → 20 settles at 6 at 30 and stays 0 at 60). Not seen to matter yet (#598).
- Mode 2's `tearing` = 1: where `graphics +0x104`, 0x513ac84 and the render list's byte at +8 end up (presumably the hsync flip when late).
- Whether the Havok behaviour and cloth updates take the task data's step.
- Whether the step-machine functions with task data of 1/30 read `delta_time`.
- `skip_wait` (+0x273, set by 0x1cf4c70 with `SprjScaleform`) and `request_0x274` (sets +0x4c, +0x4d and 60 at +0x50 of the object at 0x5540e00): what state skips the wait.
- The remaining uses of 1/30 and 30.0f that the survey did not classify.
- 0x2277780's step (0x227784b, 0x2277874): what sets the effect system's +0x2977, and whether 0xebbda0 then runs once a frame.
