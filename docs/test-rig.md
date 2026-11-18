# Test rig

`tools/rig/rig.py` runs the reference or our program with scripted gamepad input, a time limit, a memory limit and a read-only game directory. Everything a run produces goes to `analysis/rig/runs/<stamp>-<target>-<scenario>/`, and nothing there is committed.

```
tools/rig/rig.py setup                              # once: download and check the reference
tools/rig/rig.py run reference new-game-to-clinic   # the reference run
tools/rig/rig.py run bb-run --time 120              # our program, same limits, no input yet
tools/rig/rig.py run bb-run main-menu --window      # our program to the title menu
tools/rig/rig.py run reference --audio --time 100   # and bb-run the same: the sound level of the first 100 s
tools/rig/rig.py compare REF_RUN REF_RUN OUR_RUN    # its menu against two reference runs
tools/rig/rig.py run bb-run main-menu-offline --window   # on to the main menu, with the pad
tools/rig/rig.py run bb-run r5-acceptance --window --audio   # a new game to the clinic, exit, continue
tools/rig/rig.py run bb-run r5-soak --window --time 2200 --user-from RUN/user   # 25 minutes in the clinic
tools/rig/rig.py perf clinic-fight --runs 5 --bb-user RUN/user --reference-user RUN/work/user   # frame times per segment
tools/rig/rig.py perf yharnam-route --runs 5 --bb-user USER --reference-user USER   # Central Yharnam, USER = a reference run's work/user
tools/rig/rig.py perf --branch RUN... --main RUN... --reference RUN...   # the same report from finished runs
```

A run writes `target.log`, `events.csv` (what the rig did, tied to places in the log), `frames.csv` (one line per present), `memory.csv` (`peak_program_bytes` in the summary leaves out the page cache of the game's files), `threads.csv` (the user and system time of every thread of the target, once a second; `tools/rig/thread_cpu.py` reads it), `shots/*.png`, `kernel.log` and `summary.json`. For `bb-run`, `builds` and `segment_builds` in the summary spread the time its GPU thread spent on new pipelines (the `Pipeline build` and `Pipeline wait` lines in the log) over the frame intervals: the longest interval with a build in it and the most build time within one interval. The exit status is 0 when the scenario finished with no kernel GPU fault and an unchanged game directory. It is 1 after an out-of-memory kill, an unreadable kernel log, a target that exits before the scenario ends, a `shot` step without its picture, and a run with the layer in which the reference or a windowed `bb-run` presented no frame (a headless `bb-run` never presents).

Scenario steps are documented at the top of `tools/rig/scenario.py`, and scenarios live in `tools/rig/scenarios/`.

## Decisions

**A small Vulkan layer instead of MangoHud and desktop screenshots.** The layer (`tools/rig/layer/`) timestamps every `vkQueuePresentKHR` and copies the presented image on request. It works the same for any program that presents through Vulkan, so the reference and `bb-run` are measured in the same way once ours draws. It needs no window focus and takes a picture in milliseconds, where `spectacle` took up to 20 s and needed the active window. MangoHud is not used: it wrote no CSV in two runs, and picture conditions need the grabs anyway.

**Waits follow the game, not the clock.** Scenarios wait for log lines of the target (the game's file opens and system calls that shadPS4 logs) or for picture conditions on a small grey copy of the frame. Fixed sleeps remain only for menu input pacing. The log markers come from the reference, so a scenario for `bb-run` may need its own once our log differs.

**A captured frame is marked, not hidden.** A capture waits inside the present call, so `frames.csv` flags that frame and the statistics leave out the intervals around it.

**At most one picture a second.** With captures every 0.25 to 4 seconds the reference crashed around the end of the opening video in 4 of 13 runs (faults in guest memory and in emulator threads, no GPU fault). With few or no captures it was 0 of 16, and limited to one picture a second it was 5 of 5 clean. This is a correlation and the cause is not settled. A crash in the reference still ends a run with a non-zero exit status, so a failed run is visible.

**Fresh data directory per run, on purpose.** The reference uses `work/user/` inside the run directory (portable mode). A new game therefore never meets an old save. `--user-from` copies an earlier `user/` for runs that need one.

**A zenity stub answers dialogs.** On an empty data directory shadPS4 asks through SDL and zenity whether to migrate saves, and blocks until someone answers. The rig puts a stub first in `PATH` that answers "Do nothing" and logs the call to `zenity-calls.txt`. A stub that only fails makes SDL fall back to an X11 message box, which blocks in the same way.

**The pad input stays out of other programs (#543).** A uinput device is visible to the whole desktop session like a real controller, so the focused window of any program with gamepad support got the scenario's inputs during a run.
- `bb-run` gets no device at all. The rig makes a FIFO `pad` in the run directory and starts `bb-run` with `--pad-input <run>/pad`. The rig's `StreamPad` (`tools/rig/vpad.py`) writes one text line per state change (`button A 1`, `axis HY -1`, `axis RT 255`, `neutral`; format and tests in `src/common/pad_stream.hpp`), with the same waits between lines as the uinput pad has, so scenario durations stay comparable. A press and its release that arrive between two SDL pumps are coalesced for virtual joysticks, so a main thread stalled for longer than the shortest hold (120 ms) could drop a tap where uinput would queue it. In its own process `bb-run` attaches an SDL virtual gamepad (`OpenPadInput` in `src/shadps4_host/syslibs/pad_input.cpp`), applies each line as it arrives and sets `SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT` to that gamepad's ids, which wins over the environment. Its events then take the path of a real pad's through the window's gamepad handler, and nothing shows in `/proc/bus/input/devices` or to evtest and other SDL programs. It needs the window (`--window`): a headless `bb-run` has no event pump, so it never read the pad before either. `--pad-input` also works by hand: `mkfifo pad; bb-run --pad-input pad &; echo 'button A 1' > pad`. The unit tests are `tests/common/pad_stream_test.cpp`, the `PadFollowsThePadInputStream` test in `tests/shadps4/syslibs_test.cpp` and `tools/rig/test_vpad.py`.
- The reference is a black box that reads controllers from SDL, so it still needs the uinput device. It has ids of its own, vendor `0x1209` and product `0xb4b5` (name `bloodborne-linux rig pad`), not a real Xbox 360 pad's `045e:028e`. SDL does not know them, so the rig gives the reference the matching `SDL_GAMECONTROLLERCONFIG` mapping (`vpad.sdl_mapping()`, the Linux mapping of Xbox 360 pads under the pad's own GUID) and `SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x1209/0xb4b5`, so that only this pad is listed and the game gives slot 0 to it and not to a real controller (#520; `PAD_ONLY_VIRTUAL` in `tools/rig/rig.py`). Other programs that use SDL can skip the pad with `SDL_GAMECONTROLLER_IGNORE_DEVICES=0x1209/0xb4b5` and keep their real Xbox pads. Programs that don't read that variable (browsers, Steam) still see the device while a reference run lasts, so the reference's runs stay the place to be careful with a focused window.
- Both targets get `SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS`, since the pad never belongs to the focused window (`bb-run` sets the hint itself too). `bb-run` gets the same limits, logging and checks. Its XDG directories are per run and outside the checkout, because it refuses runtime directories inside the source tree. So its shader and pipeline cache starts cold in every run; `--cache-home DIR` points its `XDG_CACHE_HOME` at a directory that stays, for a warm run after a cold one. With `--window` it presents through `video_core`, so the layer measures its frames and takes its pictures as it does the reference's.

## Matching the main menu

`main-menu.scn` runs on both targets and ends on the title menu (Play Online, Play Offline), the menu the game shows with no input. `rig.py compare` decides whether our menu matches the reference's, with the tolerance the maintainer set on 2026-11-11: two reference runs give the noise floor, and ours passes within twice that floor.

**The same frame is the menu once it stands still.** The scenario waits for the game's call that hides the splash screen (the reference logs it by name, `bb-run` by its NID), then until the picture has not changed by more than one grey level for 6 s. Only the title stays still that long; the logos fade and hold for less. It then takes 8 pictures 1.5 s apart.

**Several pictures per run, compared pair by pair.** The selected entry glows, and the glow pulses. Title pictures of nine earlier reference runs differed pairwise by 0 to 0.036, depending on whether they caught the glow in the same phase, so one pair gives a floor that can be zero by chance. The floor is the mean over all 64 pairs of a picture of the first reference run with one of the second. Ours is the mean over all pairs of one of our pictures with one of either reference run.

**Mean absolute difference over the full frame.** It is the mean of |ours − reference| over every channel of every pixel, 0 to 255. Everything that differs counts, in proportion to how much it differs, and there is no threshold to tune. A count of pixels over a threshold would hinge on that threshold and treat a slightly wrong colour over the whole frame like one wrong pixel. The scenario's picture conditions use the same measure on their small grey copy.

**Same resolution, no scaling.** Both programs present 1280×720, the default window of each. `compare` refuses pictures of different sizes rather than scaling them, because scaling would add a difference of its own.

**Time to the menu is when the picture stopped changing.** The stable wait logs how long the picture had been unchanged, so the time from the start of the target to the first menu frame (within the 6 s wait's tolerance, and the one picture a second it looks at) is in `events.csv`. Ours passes when it is at most twice the mean of the two reference runs.

`main-menu-offline.scn` presses Down and A on the title and takes its pictures of the menu that follows (New Game, System, Log In). It checks that `bb-run`'s window feeds the virtual gamepad to the imported pad.

## Sound

`run --audio` starts the target with SDL's disk audio driver (`SDL_AUDIO_DRIVER=disk`), which writes what the program plays to a file in real time, in the format it opened (both targets open 48000 Hz, 8 channels, 32-bit float). The rig reads the file in 10 s windows (`tools/rig/audio.py`), puts the RMS, the peak, the silent windows and the RMS per window into `summary.json` under `audio` and deletes the file, which grows by 1.5 MB per second. The span is the run's time limit from the start of the target. The logos and the title are driven by the game and not by input, so the same span of both targets can be compared.

The disk driver replaces the device, so this measures what the game hands to SDL (AudioOut, AJM and ATRAC9), and not the device or PipeWire path.

## Character creation and the R5 acceptance run

`new-game-name.scn` goes from the title through New Game and the opening video to the character editor, opens the name dialog (the game's ImeDialog), types one letter with the pad, presses Done and Finish, and ends once the game saves. A `log` step matches only text logged after the previous `log` step's match, so the final save step cannot match the mounts at game start.

`new-game-to-clinic.scn` is the reference run: from a fresh data directory through character creation and the intro to first control in the clinic. It waits for log text that either target prints.

`r5-acceptance.scn` goes from the title through New Game, the opening movie, the name dialog and the intro to the clinic, moves the character with the stick, exits to the title through Options, System and Exit Game (the game saves as it exits), and continues from the save back into the clinic. `r5-soak.scn` continues from that save (`--user-from` takes the `user/` directory the acceptance run leaves in its run directory, for `bb-run` as for the reference) and plays for 25 minutes from there: the stick back and forth, the camera and the lock-on button, a wait in each round for the picture to change after the stick moves (a frozen picture fails the run), and one picture a minute. The stick walks the character out of the starting room, so this is 25 minutes in the game after Continue, starting in the clinic. `r5-soak` ran on both targets, since its waits use the picture and log text the two share. `r5-acceptance` ran on `bb-run` only.

**The scenarios wait on log lines and pictures, and not all dialogs come every time.** The notices after Play Offline ("Unable to connect to the network", "Starting game in offline mode") and the one about an unclean end of the last session appear in different numbers from run to run, and a press that meets none of them lands on a menu entry. The acceptance run takes the save mount that follows the last notice as the sign that the menu is up (`lognow` makes a `log` step ignore everything logged until then, since the mounts of earlier phases would match at once), and the soak presses A six times three seconds apart, which is harmless once the game is running. A loading page stands still like the title, so "loaded" is a still picture after a fixed 10 s for the page to go, and the shots show which one it was.

**`repeat COUNT` and `lognow` are scenario steps** (`tools/rig/scenario.py`). `repeat` writes the lines to the matching `end` out COUNT times and replaces `{i}` by the number, so a shot can be named per round. A mark inside it still can't be used twice.

## Frame times per segment (`rig.py perf`)

`perf SCENARIO --runs N` runs the scenario N times on `bb-run` of the checkout (the column `branch`), N times on the reference and, with `--main-bb-run BINARY`, N times on a `bb-run` built from `main` (the column `main`), one run at a time, then prints the report. Each target needs a save: `--bb-user` takes the `user/` directory of an `r5-acceptance` run and `--reference-user` that of a reference run (`work/user/`). Without `--runs` it reports on finished runs given with `--branch`, `--main` and `--reference`. `--frame-rate 30|60` passes the target frame rate to the `bb-run` runs of both `bb-run` columns, `branch` and `main`, never to the reference; a `main` binary built before `bb-run` had the option (#514) refuses it, so leave it out or build `main` again. `rig.py run bb-run --frame-rate 60` does the same for one run, and `run reference` refuses it, like `--log`, `--perf` and `--bb-run-args`. `--bb-run-args` passes any other `bb-run` options, shell-quoted in one argument (`--bb-run-args="--window-size 2560x1440 --fullscreen --fsr"`). `--bb-run-config FILE` makes `bb-run` take every setting from that config file (`--config`, docs/design/launcher.md), with no `--game-dir` and no `--profile`, and removes `BB_GAME_DIR` from its environment; the rig itself still reads the game folder for its checks. The `run reference` target refuses it. The rig's `VK_LAYER_PATH` lists its own layer first and then the caller's `VK_LAYER_PATH` and `VK_ADD_LAYER_PATH` and the system's layer directories, so `--validation` finds an installed validation layer; one that is not installed can be given with `VK_ADD_LAYER_PATH`. A run that did not finish its scenario (its `events.csv` does not end with `end,scenario finished`) is left out with a message, since it gives no metric. `--only GLOB` limits the report to segments by name.

Per segment and column the report gives the median over the runs of the p50, p95, p99 and maximum frame interval and of the stutters (intervals over twice the segment's median), and the spread of the p95 and of the p99 (largest minus smallest of the runs). A column is flagged where its p95 or its p99 is above the reference's by more than the larger of the two spreads of that percentile (#528 added the p95: more than 5 % of the frames missing their 33.3 ms slot moves it to 50 ms, which the p99 rule does not see when both targets sit on the 50 ms step). A flagged segment gets its own issue in R8.2. The p99 of a 30 fps segment is 33.4 ms or 50 ms, since the game paces frames to the 33.33 ms grid and one late present moves a frame to the next 16.7 ms step, so the spread is 0 or a multiple of 16.7 ms.

`clinic-fight.scn` is the gameplay route in the clinic: Continue from the save, then segments named `play-*` of standing (20 s), walking, camera turns, dodging, attacking (`RB`, `R2`), lock-on with strafing, attacking and dodging, and walking again, about 220 s of play in all. The hunter has no weapon in the clinic, so the attack presses show an unarmed hunter at best; the dodge, the lock-on and the walk are real. `clinic-door.scn` continues from a save at the clinic's door and walks out for 25 s (#598).

### Central Yharnam

`yharnam-route.scn` continues from a save at the great bonfire in Central Yharnam, made by playing the reference by hand (not committed, under `analysis/rig/manual/`). Both targets load it: `--user-from` (and `--bb-user`) takes the reference's `user/` directory (`home/ID/savedata/TITLE/SPRJ0005`) for `bb-run` too, and `rig.py` copies the save files into the layout `bb-run` uses (`savedata/SPRJ0005`). The segments are `play-stand` (15 s), `play-walk` (out of the alcove into the street with the fire, under the arch), `play-camera`, `play-sprint` (`B` held 2.5 s, back and forth), `play-dodge`, `play-attack`, `play-lockon` and `play-walk-again`, about 235 s of play. No enemy is near on the way, so it covers the open street with fire and fog but not fighting. The camera swings as the hunter moves, so the walk ends in one of two places from run to run.

`yharnam-fight.scn` continues from a second hand-made save just before a group of living enemies (`analysis/rig/manual/yharnam-enemies/work/user`). The hunter walks out until they notice (`play-walk`), locks on and backs toward the arch (`play-lockon`), and fights in short exchanges of a swing and a roll (`play-fight-first`, `play-fight-strong` with `R2`, `play-fight-last`), with a blood vial between them (`play-heal`, `play-heal-again`) and a sprint at the end (`play-sprint`). About 100 s of play. The route is blind, so how long the enemies stay in contact differs between runs. It ends its load wait with `expect_level min=33 max=45`, a step that fails the run when the picture's mean luma is outside the band: in about 1 of 5 runs an enemy killed the hunter during the wait, and the run measured a loading page and the lantern route instead (#538). The band fits this save's lighting only; another save needs its own.

`yharnam-death.scn` continues from the same save, walks 4 s toward the enemies and stands until they kill the hunter. YOU DIED, the load and the respawn at the lantern follow, and the shots after it show the hunter from behind. The scenario has no check step of its own: its shots are compared by eye against a reference run, the hunter and his weapon whole and no large flat polygon of one colour anywhere (#537).

## Profiling `bb-run` (#513)

Tracy is gone, so a slow frame is found with three tools: `perf` for where the threads spend CPU time, frame counters for how much each part of the GPU thread's frame took, and the thread times of every run for what each thread costs per frame.

### perf

```
tools/rig/rig.py run bb-run yharnam-route --window --user-from USER --perf --cache-home DIR
python3 tools/rig/perfmap.py --perf-data RUN/perf.data     # writes /tmp/perf-<pid>.map
perf report -i RUN/perf.data --no-children --sort comm,symbol -g none --stdio
```

`--perf` runs `bb-run` under `perf record -k mono -e cpu-clock:u -F 499 --call-graph fp --user-callchains` into the run's `perf.data`. `-k mono` stamps the samples with `CLOCK_MONOTONIC`, the clock of `events.csv` and `frames.csv`, so `perf report --time START,END` (seconds with nine decimals, from the `mono_ns` of two marks) takes one segment of the scenario. With `kernel.perf_event_paranoid=2` (no sysctl change, no sudo) only user-space samples of our own process work, and the callchain event fails to open without `--user-callchains` ("Failure to open any events"). The `relwithdebinfo` preset builds with `BB_FRAME_POINTERS=ON` (`-fno-omit-frame-pointer`), so `--call-graph fp` unwinds our code and the system libraries it calls; guest code has no frame pointers to follow, so a chain stops at the first guest frame. Without a call chain, `--sort comm` is the split by thread (the guest's thread names, `shadPS4:GpuComm` for the GPU thread, `GXWorker:N`, `game main`).

`perfmap.py` writes the file perf reads for code outside any file: the guest functions of `symbols/functions.csv` at the eboot's load base (`loader::default_base`, 0x10000000, plus the link address). The file has only the functions named so far (a handful), so guest samples mostly show as raw addresses `0x1xxxxxxx`; subtract 0x10000000 for the link address in Ghidra. Samples are CPU time only: a thread that sleeps (a lock, a wait for the GPU) shows nothing, which is what the frame counters are for.

Run it under `flock` when other agents use the GPU, and keep a perf run apart from a counter run, so the counters do not show up in the samples. `perf` itself adds 2 to 4 % to the GPU thread (#533).

### Thread times

Every run writes `threads.csv`: once a second the rig reads the user and system time of every thread in the target's cgroup from `/proc` (in clock ticks of 10 ms). `tools/rig/thread_cpu.py RUN [--segment GLOB] [--thread GLOB]` divides each thread's CPU time between the samples at or before a segment's two marks (up to a second early) by the presents in the same span: CPU ms per frame, with the system time in brackets, and numbered siblings (`GXWorker:1` to `:5`) summed under one name. A thread that starts or ends inside the span is left out, so a sum over short-lived workers reads low. It costs the target nothing, works on the reference as on `bb-run`, and counts kernel time, which `cpu-clock:u` samples never see. Over a 10 s segment of 300 frames, the tick and the one-second sampling give it a resolution of about 0.05 ms per frame. It is the measure for changes that make the GPU thread cheaper: `thread_cpu.py RUN --thread shadPS4:GpuComm`.

### Frame counters

`bb-run --log profile=debug` (`rig.py run bb-run SCENARIO --log profile=debug`) turns on the counters of `src/common/frame_profile.hpp`; without it a disabled counter costs about 1 ns (a load and a branch, inline). Once per frame (the guest's submit-done) the log gets one line:

```
debug [...] profile: Frame profile: at <ns> ns, <us> us since the last, commands=16372 us/74, draw=15810 us/1364, ...
```

Each part is `part=<us> us/<calls>` of the time since the previous line, summed over all threads. Times are inclusive (a part inside another counts in both), and a part with no call is left out. The parts:

| part | what |
|---|---|
| `commands` | GPU thread running the command buffers (PM4 processing, all of the below that runs on it) |
| `draw` | `Rasterizer` draw and dispatch calls |
| `pipeline` | pipeline lookup (shader translation and waiting for a build are inside) |
| `bind` | binding the resources of a draw: `buffer` and `texture` are inside |
| `buffer`, `sync`, `resident` | buffer cache: obtaining a buffer; copying CPU-modified guest memory into it; making sparse blocks resident |
| `texture`, `upload`, `detile`, `upload_new` | texture cache work of a draw; refreshing a dirty image from guest memory; detiling it; the share of `upload` that is the first upload of a cache image object (a new texture, or one evicted and created again; not a re-upload of a changed image) |
| `render`, `vertex`, `state` | render pass setup; vertex and index buffer binding; dynamic state and descriptor writes |
| `copy`, `irq`, `queue` | buffer copies and fills; interrupt handlers signalled on the GPU thread; callbacks queued to it |
| `fault` | write-tracking SIGSEGV handler, whole, on any guest thread |
| `inval_buffer`, `inval_texture` | the buffer and texture caches' share of a fault |
| `protect` | `mprotect` calls of write tracking (from a fault or from a cache sync) |
| `wait`, `submit`, `present` | the scheduler waiting for the GPU; end-of-frame housekeeping; the presenter thread's present call |
| `flip_wait` | a flip request waiting from its queueing to the vblank that took it |

The counters are for finding the big things. A scope reads the CPU's time-stamp counter twice and adds to counters of its own thread, about 20 ns in a tight loop, and `log_frame` collects the threads' counters once per frame and scales the ticks to ns against `CLOCK_MONOTONIC`. A `play-walk` frame of `yharnam-route` has about 47 000 scopes, so they add under 1 ms to it; the GPU thread's CPU time per frame with and without them differs by less than the spread between runs (#594). Counter numbers from runs before #594 read too high in proportion to a part's calls: a scope then cost about 120 ns. For the exact cost of a part use `perf` or `thread_cpu.py`. The counters need a time-stamp counter that runs at a constant rate on all cores (`constant_tsc` and `nonstop_tsc` in `/proc/cpuinfo`). A thread keeps its counters after it exits; past 256 threads that ever ran a scope, the rest add to shared atomics, slower and still counted. The fault part is the one that runs in a signal handler: it reads the time-stamp counter and adds to its thread's counters, with no lock and no allocation.

`tools/rig/frame_profile.py RUN [--segment GLOB]` reads a run's log, `events.csv` and `frames.csv` and prints for each scenario segment: the frame interval of the GPU thread (the submit-done to submit-done time), the slowest frames with their biggest parts (`build` is the `Pipeline build` and `Pipeline wait` time overlapping the frame), the parts' extra cost in a slow frame over a normal one, a normal frame, and the late presents (a present interval over 1.5 times the median): the time from the submit-done before the present to the present, and that frame's `commands`. Read it with `--cache-home` of an earlier run: a cold cache puts shader and pipeline builds into the slow frames and hides the rest.

### What profiling decided

- **Holes of direct memory are not read (#531).** A buffer sync of a range the game never wrote read the holes of the direct memory memfd page by page, and each read faulted in and cleared a page. `MemoryManager::CopySparseMemory` writes zeros for the holes of copies of 64 KiB or more without reading them (`lseek` `SEEK_DATA`).
- **The staging ring keeps its first 12 blocks (#531).** After the loading screens it had freed its blocks, and a later upload burst paid for new 16 MiB blocks and their first-touch faults in the middle of a frame.
- **Vertex syncs stay whole (#531).** Copying only the vertices a draw fetches saved about 4 ms in the one upload frame of the Yharnam walk but cost about 7 ms in every other frame, because each draw then scanned its indices again over pages no draw had synced. Not kept.
- **Texture bursts are new content (#532).** The upload bursts of the walk are images the cache sees for the first time, as the game streams an area in. They have to be uploaded, so nothing is deferred. `frame_profile.py` lists frames with over 50 refreshes and how many were first uploads.
- **The work semaphore is polled only when an operation waits for it (#589).** Every draw queried the timeline semaphore with an `ioctl`, about 2 to 3 ms of system time per frame. #590 and #591 cut other per-draw costs found in the same profile (#533); #592 and #593 are open.
- **`game main` busy-waits in the frame pacer**, about half of all CPU samples. It costs a core but is not on the frame's critical path (`docs/design/frame-rate.md`).
