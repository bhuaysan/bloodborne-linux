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

`run bb-run` takes `build/<preset>/src/tools/bb-run` of the checkout it runs from, or of the main checkout when a worktree has no build of its own; it says so on stderr and records `bb_run` (`binary`, `from_main_checkout`, `stale`) in `summary.json`. It refuses a binary older than the last commit of the checkout it runs from (even a docs-only commit or a rebase), before anything starts, since a stale build tests old code (#477): build the preset, or pass `--allow-stale` to run it anyway. `--bb-run BINARY` is never checked.

A run writes `target.log`, `events.csv` (what the rig did, tied to places in the log), `frames.csv` (one line per present), `memory.csv` (`peak_program_bytes` in the summary leaves out the page cache of the game's files; `peak_maps` is the most memory mappings one process of the target had, see "Distributions"), `threads.csv` (the user and system time of every thread of the target, once a second; `tools/rig/thread_cpu.py` reads it), `shots/*.png`, `kernel.log` and `summary.json`. For `bb-run`, `builds` and `segment_builds` in the summary spread the time its GPU thread spent on new pipelines (the `Pipeline build` and `Pipeline wait` lines in the log) over the frame intervals: the longest interval with a build in it and the most build time within one interval. The exit status is 0 when the scenario finished with no kernel GPU fault and an unchanged game directory. It is 1 after an out-of-memory kill, an unreadable kernel log, a target that exits before the scenario ends, a `shot` step without its picture, and a run with the layer in which the reference or a windowed `bb-run` presented no frame (a headless `bb-run` never presents).

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
- Both targets get `SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS`, since the pad never belongs to the focused window (`bb-run` sets the hint itself too). `bb-run` gets the same limits, logging and checks. Its user folder (`BB_USER_DIR`) is per run and outside the checkout, because it refuses runtime directories inside the source tree. So its shader and pipeline cache, and the Mesa shader cache below it, start cold in every run; `--cache-home DIR` points its cache (`BB_CACHE_DIR`) at a directory that stays, for a warm run after a cold one. With `--window` it presents through `video_core`, so the layer measures its frames and takes its pictures as it does the reference's.

## Matching the main menu

`main-menu.scn` runs on both targets and ends on the title menu (Play Online, Play Offline), the menu the game shows with no input. `rig.py compare` decides whether our menu matches the reference's, with the tolerance the maintainer set on 2026-11-11: two reference runs give the noise floor, and ours passes within twice that floor.

**The same frame is the menu once it stands still.** The scenario waits for the game's call that hides the splash screen (the reference logs it by name, `bb-run` by its NID), then until the picture's mean level reaches 12 and has not changed by more than one grey level for 6 s. The logos are dark (mean level below 8, the title 23) and hold about 4 s each, but under machine load a hold can pass 6 s, so stillness alone took a logo for the title on 2026-11-22 and the menu presses landed too early (#791). Every scenario that starts at the logos waits this way. It then takes 8 pictures 1.5 s apart.

**Several pictures per run, compared pair by pair.** The selected entry glows, and the glow pulses. Title pictures of nine earlier reference runs differed pairwise by 0 to 0.036, depending on whether they caught the glow in the same phase, so one pair gives a floor that can be zero by chance. The floor is the mean over all 64 pairs of a picture of the first reference run with one of the second. Ours is the mean over all pairs of one of our pictures with one of either reference run.

**Mean absolute difference over the full frame.** It is the mean of |ours − reference| over every channel of every pixel, 0 to 255. Everything that differs counts, in proportion to how much it differs, and there is no threshold to tune. A count of pixels over a threshold would hinge on that threshold and treat a slightly wrong colour over the whole frame like one wrong pixel. The scenario's picture conditions use the same measure on their small grey copy.

**Same resolution where possible, else scaled down.** Both programs present 1280×720, the default window of each, so menu runs compare unscaled. The rig's shots are the presented swapchain image, so their size is the window's (`--window-size` or `--fullscreen`), not the game's `--resolution` (#515): to see a 2560×1440 frame scaled down, run it in a larger window than the 1x runs. A window the window manager shrinks to fit the screen (3840×2160 asked, 1920×1006 given) is no longer 16:9. Runs with shots of different sizes are scaled: `compare` averages every shot by area down to the smallest size among the three runs, and refuses shots whose aspect ratios differ by more than 1%. Scaling adds a small difference of its own, so compare scaled runs only against references taken at the same sizes, or read the ratio with that in mind.

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

`perf SCENARIO --runs N` runs the scenario N times on `bb-run` of the checkout (the column `branch`), N times on the reference and, with `--main-bb-run BINARY`, N times on a `bb-run` built from `main` (the column `main`), one run at a time, then prints the report. Each target needs a save: `--bb-user` takes the `user/` directory of an `r5-acceptance` run and `--reference-user` that of a reference run (`work/user/`). Without `--runs` it reports on finished runs given with `--branch`, `--main` and `--reference`. `--frame-rate 30|60|120` passes the target frame rate to the `bb-run` runs of both `bb-run` columns, `branch` and `main`, never to the reference; a `main` binary built before `bb-run` had the option (#514) refuses it, so leave it out or build `main` again. `rig.py run bb-run --frame-rate 60` does the same for one run, and `run reference` refuses it, like `--log`, `--perf` and `--bb-run-args`. `--bb-run-args` passes any other `bb-run` options, shell-quoted in one argument (`--bb-run-args="--window-size 2560x1440 --fullscreen --fsr"`). `--bb-run-config FILE` makes `bb-run` take every setting from that config file (`--config`, docs/design/launcher.md), with no `--game-dir` and no `--profile`, and removes `BB_GAME_DIR` from its environment; the rig itself still reads the game folder for its checks. The `run reference` target refuses it. The rig's `VK_LAYER_PATH` lists its own layer first and then the caller's `VK_LAYER_PATH` and `VK_ADD_LAYER_PATH` and the system's layer directories, so `--validation` finds an installed validation layer; one that is not installed can be given with `VK_ADD_LAYER_PATH`. A run that did not finish its scenario (its `events.csv` does not end with `end,scenario finished`) is left out with a message, since it gives no metric. `--only GLOB` limits the report to segments by name.

Per segment and column the report gives the median over the runs of the p50, p95, p99 and maximum frame interval and of the stutters (intervals over twice the segment's median), and the spread of the p95 and of the p99 (largest minus smallest of the runs). A column is flagged where its p95 or its p99 is above the reference's by more than the larger of the two spreads of that percentile (#528 added the p95: more than 5 % of the frames missing their 33.3 ms slot moves it to 50 ms, which the p99 rule does not see when both targets sit on the 50 ms step). A flagged segment gets its own issue in R8.2. The p99 of a 30 fps segment is 33.4 ms or 50 ms, since the game paces frames to the 33.33 ms grid and one late present moves a frame to the next 16.7 ms step, so the spread is 0 or a multiple of 16.7 ms.

`clinic-fight.scn` is the gameplay route in the clinic: Continue from the save, then segments named `play-*` of standing (20 s), walking, camera turns, dodging, attacking (`RB`, `R2`), lock-on with strafing, attacking and dodging, and walking again, about 220 s of play in all. The hunter has no weapon in the clinic, so the attack presses show an unarmed hunter at best; the dodge, the lock-on and the walk are real. `clinic-door.scn` continues from a save at the clinic's door and walks out for 25 s (#598).

### Central Yharnam

`yharnam-route.scn` continues from a save at the great bonfire in Central Yharnam, made by playing the reference by hand (not committed, under `analysis/rig/manual/`). Both targets load it: `--user-from` (and `--bb-user`) takes the reference's `user/` directory (`home/ID/savedata/TITLE/SPRJ0005`) for `bb-run` too, and `rig.py` copies the save files into the layout `bb-run` uses (`savedata/SPRJ0005`). The segments are `play-stand` (15 s), `play-walk` (out of the alcove into the street with the fire, under the arch), `play-camera`, `play-sprint` (`B` held 2.5 s, back and forth), `play-dodge`, `play-attack`, `play-lockon` and `play-walk-again`, about 235 s of play. No enemy is near on the way, so it covers the open street with fire and fog but not fighting. The camera swings as the hunter moves, so the walk ends in one of two places from run to run.

`yharnam-fight.scn` continues from a second hand-made save just before a group of living enemies (`analysis/rig/manual/yharnam-enemies/work/user`). The hunter runs up to them and swings once to draw them (`play-walk`), locks on and backs up the steps into the arch (`play-lockon`), and fights two short exchanges of a swing and a roll per segment (`play-fight-first`, `play-fight-strong` with `R2`, `play-fight-last`), with a blood vial between them (`play-heal`, `play-heal-again`) and a sprint at the end (`play-sprint`). About 80 s of play. Until #654 the route turned back after 3 s, before the enemies noticed the hunter, so it measured the street with enemies but no fight. The route is blind, so how long the enemies stay in contact differs between runs, and the weak hunter dies in about half of them: leave out a run whose shots show YOU DIED or a loading page. It ends its load wait with `expect_level min=33 max=45`, a step that fails the run when the picture's mean luma is outside the band: in about 1 of 5 runs an enemy killed the hunter during the wait, and the run measured a loading page and the lantern route instead (#538). The band fits this save's lighting only; another save needs its own.

`yharnam-death.scn` continues from the same save, walks 4 s toward the enemies and stands until they kill the hunter. YOU DIED, the load and the respawn at the lantern follow, and the shots after it show the hunter from behind. The scenario has no check step of its own: its shots are compared by eye against a reference run, the hunter and his weapon whole and no large flat polygon of one colour anywhere (#537).

### Heavy areas at 30, 60 and 120 (#654)

Measured on 2026-11-23 to decide which thread bounds a frame at 120 FPS. No save later than Central Yharnam exists, so the maintainer limited the measurement to the existing routes: `clinic-fight`, `yharnam-route` and `yharnam-fight`. `bb-run` was a `relwithdebinfo` build of `main` at `35b4567`, run with `--window` (1280x720), a warm `--cache-home` (one warm-up run per route first) and `--perf`. It made three runs per route at 30, 60 and 120, and one more run at 120 with `--log profile=debug` for the counters, kept apart from the perf runs. The reference made three runs per route at 30. `yharnam-fight` ran at 120 only, with the route as changed by #654, which now draws the group into a fight. Of five runs, two ended with the hunter's death and are left out. The counter run died after `play-heal`, so its counters stop there.

Before each run the machine had no compiler and no difftest running, and the load from other programs stayed under 1.5 cores. Bursts from another program still came every 30 to 40 minutes, up to 14.7 cores for a few minutes. A sampler of the CPU time outside the rig's scope (every 5 s) marked them, and a segment of a run is left out where that load went over 4 cores. A fourth run at 30 and at 120 kept at least three runs per segment, except where the table cell says otherwise below. The numbers are medians over the runs. Times are in ms.

**Frame times** (present interval p50 / p95 / p99; the reference only at 30):

| route | segment | reference 30 (p50/p95/p99) | 30 | 60 | 120 | FPS at 120 |
|---|---|---|---|---|---|---|
| clinic-fight | play-stand | 33.3 / 33.4 / 50.1 | 33.3 / 33.4 / 33.4 | 16.7 / 16.7 / 16.7 | 8.3 / 9.1 / 14.0 | 117 |
| clinic-fight | play-walk | 33.3 / 50.0 / 50.1 | 33.3 / 33.4 / 33.4 | 16.7 / 16.7 / 16.7 | 8.3 / 9.4 / 16.4 | 119 |
| clinic-fight | play-camera | 33.3 / 33.5 / 50.0 | 33.3 / 33.4 / 33.4 | 16.7 / 16.7 / 16.7 | 8.3 / 9.0 / 12.4 | 119 |
| clinic-fight | play-dodge | 33.3 / 33.4 / 50.0 | 33.3 / 33.4 / 33.4 | 16.7 / 16.7 / 16.7 | 8.3 / 8.4 / 8.4 | 120 |
| clinic-fight | play-attack | 33.3 / 33.4 / 33.4 | 33.3 / 33.4 / 33.4 | 16.7 / 16.7 / 16.7 | 8.3 / 8.4 / 8.4 | 120 |
| clinic-fight | play-lockon | 33.3 / 33.4 / 33.5 | 33.3 / 33.4 / 33.4 | 16.7 / 16.7 / 16.7 | 8.3 / 8.4 / 8.4 | 120 |
| clinic-fight | play-walk-again | 33.3 / 33.4 / 33.5 | 33.3 / 33.4 / 33.4 | 16.7 / 16.7 / 16.7 | 8.3 / 8.4 / 9.7 | 119 |
| yharnam-route | play-stand | 33.3 / 33.4 / 33.5 | 33.3 / 33.4 / 33.4 | 16.7 / 16.7 / 16.7 | 8.3 / 8.4 / 8.8 | 119 |
| yharnam-route | play-walk | 33.3 / 33.4 / 50.1 | 33.3 / 33.4 / 33.5 | 16.7 / 16.7 / 18.6 | 9.3 / 14.1 / 17.7 | 103 |
| yharnam-route | play-camera | 33.3 / 33.4 / 50.1 | 33.3 / 33.4 / 33.4 | 16.7 / 16.7 / 16.7 | 8.3 / 9.7 / 16.7 | 115 |
| yharnam-route | play-sprint | 33.3 / 33.5 / 50.0 | 33.3 / 33.4 / 34.0 | 16.7 / 16.7 / 16.7 | 8.4 / 16.6 / 17.7 | 109 |
| yharnam-route | play-dodge | 33.3 / 33.5 / 50.0 | 33.3 / 33.4 / 33.4 | 16.7 / 16.7 / 16.7 | 8.3 / 11.3 / 17.7 | 109 |
| yharnam-route | play-attack | 33.3 / 33.4 / 41.8 | 33.3 / 33.4 / 34.4 | 16.7 / 16.7 / 16.8 | 8.3 / 10.7 / 17.7 | 112 |
| yharnam-route | play-lockon | 33.3 / 33.4 / 50.0 | 33.3 / 33.4 / 33.5 | 16.7 / 16.7 / 16.8 | 8.3 / 10.1 / 16.6 | 114 |
| yharnam-route | play-walk-again | 33.3 / 33.4 / 33.6 | 33.3 / 33.4 / 34.4 | 16.7 / 16.7 / 16.7 | 8.3 / 9.7 / 10.8 | 116 |
| yharnam-fight | play-walk | – | – | – | 9.4 / 16.6 / 17.7 | 103 |
| yharnam-fight | play-lockon | – | – | – | 8.4 / 10.5 / 17.5 | 110 |
| yharnam-fight | play-fight-first | – | – | – | 8.3 / 9.1 / 10.8 | 117 |
| yharnam-fight | play-heal | – | – | – | 8.3 / 10.0 / 11.0 | 115 |
| yharnam-fight | play-fight-strong | – | – | – | 8.5 / 16.7 / 17.7 | 104 |
| yharnam-fight | play-heal-again | – | – | – | 9.4 / 16.7 / 17.7 | 93 |
| yharnam-fight | play-fight-last | – | – | – | 9.0 / 13.5 / 17.2 | 105 |
| yharnam-fight | play-sprint | – | – | – | 8.3 / 10.2 / 16.7 | 115 |

Left out by the load rule: `yharnam-route` at 120 has two runs in `play-attack`, and the reference in Yharnam has two runs in `play-dodge` and `play-attack`.

**CPU per present at 120** (`thread_cpu.py`: the game's main loop `game main`, its render thread `GXRenderThread`, its five `GXWorker`s summed, our GPU thread `shadPS4:GpuComm` and recording thread `shadPS4:GpuRec`) and the GPU's busy time per present (`gpu.csv`):

| route | segment | main loop | render thread | GX workers (5) | GPU thread | recording | GPU |
|---|---|---|---|---|---|---|---|
| clinic-fight | play-stand | 5.4 | 0.6 | 8.5 | 7.4 | 3.2 | 8.3 |
| clinic-fight | play-walk | 4.9 | 0.6 | 6.3 | 5.7 | 2.5 | 7.3 |
| clinic-fight | play-camera | 4.7 | 0.6 | 5.2 | 4.9 | 2.1 | 6.8 |
| clinic-fight | play-dodge | 4.3 | 0.6 | 3.6 | 3.6 | 1.6 | 6.3 |
| clinic-fight | play-attack | 4.2 | 0.6 | 3.5 | 3.5 | 1.5 | 6.5 |
| clinic-fight | play-lockon | 4.2 | 0.6 | 3.3 | 3.3 | 1.5 | 5.9 |
| clinic-fight | play-walk-again | 5.0 | 0.6 | 6.0 | 5.5 | 2.3 | 7.2 |
| yharnam-route | play-stand | 5.6 | 0.6 | 7.8 | 6.6 | 2.6 | 7.3 |
| yharnam-route | play-walk | 4.8 | 0.7 | 9.3 | 8.8 | 3.3 | 8.9 |
| yharnam-route | play-camera | 5.3 | 0.7 | 7.7 | 7.0 | 2.8 | 7.8 |
| yharnam-route | play-sprint | 4.8 | 0.7 | 8.7 | 8.3 | 3.2 | 8.5 |
| yharnam-route | play-dodge | 5.2 | 0.6 | 8.1 | 7.7 | 3.0 | 8.2 |
| yharnam-route | play-attack | 5.2 | 0.6 | 8.3 | 7.5 | 3.0 | 7.7 |
| yharnam-route | play-lockon | 5.3 | 0.6 | 7.8 | 7.1 | 2.8 | 7.6 |
| yharnam-route | play-walk-again | 5.3 | 0.6 | 8.2 | 7.4 | 2.9 | 7.6 |
| yharnam-fight | play-walk | 4.6 | 0.7 | 9.1 | 9.0 | 3.3 | 8.9 |
| yharnam-fight | play-lockon | 5.2 | 0.7 | 9.1 | 8.3 | 3.2 | 8.4 |
| yharnam-fight | play-fight-first | 5.4 | 0.7 | 7.2 | 6.7 | 2.6 | 7.3 |
| yharnam-fight | play-heal | 5.1 | 0.7 | 7.8 | 7.6 | 3.0 | 7.9 |
| yharnam-fight | play-fight-strong | 4.4 | 0.7 | 8.0 | 8.6 | 3.2 | 8.2 |
| yharnam-fight | play-heal-again | 4.2 | 0.7 | 8.7 | 9.0 | 3.2 | 9.1 |
| yharnam-fight | play-fight-last | 4.4 | 0.6 | 7.2 | 7.5 | 2.8 | 7.6 |
| yharnam-fight | play-sprint | – | – | – | – | – | 7.0 |

**The heaviest segments are in Central Yharnam at 120:** the fight's `play-heal-again` (p50 9.4 ms, 93 FPS, with a wide spread between runs), `play-walk` of both routes (103 FPS), the fight's `play-fight-strong` and `play-fight-last` (104 and 105 FPS), and `play-sprint`, `play-dodge` and `play-attack` of `yharnam-route` (109 to 112 FPS). The clinic holds 120 in every segment except a few late frames in `play-stand` to `play-camera`. Every segment holds 60: the highest p99 at 60 is 18.65 ms (`yharnam-route` `play-walk`, one run), under the 18.8 ms limit, so no segment needs a follow-up issue for its 60 FPS slot. At 30, `bb-run` stays within 33.4 to 34.4 ms p99 where the reference often shows 50 ms.

**What bounds a frame there is the GPU thread.** In the slow Yharnam segments with counters, the GPU thread's `commands` take 9.2 to 10.2 ms of a normal frame (table below), more than the 8.33 ms slot. Its `wait` (the scheduler waiting for the GPU) is 0.00, so it never waits for the GPU. The frame rate matches 1/`commands` (9.7 ms a frame at 103 FPS). The game's main loop uses 4.4 to 5.6 ms of CPU per present and its render thread 0.7 ms, so neither bounds a frame. The GPU is busy 7.6 to 9.1 ms per present in the same segments, up to 92 % of the time. That share is not a bound by itself: the clinic's `play-stand` holds 117 to 120 FPS at 8.3 ms per present (about 97 % busy), because the GPU's clock follows its load. Once the GPU thread is cheaper, the GPU is the next thing to check, with its clock read alongside the busy share.

**GPU thread counters at 120** (one `--log profile=debug` run per route, a normal frame, inclusive parts; `fault` and `protect` count all threads):

| route | segment | `commands` | `pipeline` | `bind` | `buffer` | `sync` | `texture` | `upload` | `detile` | `render` | `vertex` | `state` | `wait` | `fault` | `protect` | pipeline+vertex+state |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| clinic-fight | play-stand | 7.86 | 1.24 | 2.20 | 1.57 | 0.77 | 1.15 | 0.03 | 0.02 | 0.76 | 0.66 | 0.51 | 0.00 | 4.76 | 1.94 | 31 % |
| clinic-fight | play-walk | 6.28 | 1.00 | 1.69 | 1.25 | 0.63 | 0.89 | 0.02 | 0.02 | 0.58 | 0.59 | 0.40 | 0.00 | 3.73 | 1.49 | 32 % |
| clinic-fight | play-camera | 5.22 | 0.83 | 1.35 | 1.06 | 0.56 | 0.71 | 0.02 | 0.01 | 0.46 | 0.49 | 0.33 | 0.00 | 2.98 | 1.26 | 32 % |
| clinic-fight | play-dodge | 3.74 | 0.61 | 0.94 | 0.72 | 0.38 | 0.52 | 0.01 | 0.01 | 0.32 | 0.32 | 0.23 | 0.00 | 2.07 | 0.86 | 31 % |
| clinic-fight | play-attack | 3.69 | 0.60 | 0.92 | 0.71 | 0.39 | 0.51 | 0.01 | 0.01 | 0.31 | 0.31 | 0.23 | 0.00 | 2.01 | 0.88 | 31 % |
| clinic-fight | play-lockon | 3.45 | 0.57 | 0.84 | 0.67 | 0.38 | 0.46 | 0.01 | 0.01 | 0.28 | 0.29 | 0.22 | 0.00 | 1.78 | 0.83 | 31 % |
| clinic-fight | play-walk-again | 4.38 | 0.70 | 1.12 | 0.87 | 0.47 | 0.60 | 0.01 | 0.01 | 0.37 | 0.37 | 0.28 | 0.00 | 2.35 | 1.02 | 31 % |
| yharnam-route | play-stand | 6.95 | 1.02 | 2.01 | 1.42 | 0.73 | 1.16 | 0.00 | 0.00 | 0.67 | 0.69 | 0.45 | 0.00 | 4.53 | 2.53 | 31 % |
| yharnam-route | play-walk | 9.53 | 1.48 | 2.87 | 1.87 | 0.85 | 1.60 | 0.03 | 0.01 | 0.89 | 0.97 | 0.63 | 0.00 | 5.30 | 2.82 | 32 % |
| yharnam-route | play-camera | 7.72 | 1.18 | 2.23 | 1.57 | 0.77 | 1.24 | 0.01 | 0.01 | 0.71 | 0.79 | 0.50 | 0.00 | 4.47 | 2.47 | 32 % |
| yharnam-route | play-sprint | 9.82 | 1.55 | 2.94 | 1.93 | 0.88 | 1.62 | 0.01 | 0.01 | 0.91 | 1.00 | 0.64 | 0.00 | 5.51 | 2.83 | 32 % |
| yharnam-route | play-dodge | 9.94 | 1.57 | 2.99 | 1.95 | 0.90 | 1.65 | 0.02 | 0.01 | 0.93 | 0.99 | 0.66 | 0.00 | 5.97 | 3.18 | 32 % |
| yharnam-route | play-attack | 10.19 | 1.62 | 3.10 | 1.97 | 0.90 | 1.71 | 0.01 | 0.01 | 0.95 | 1.00 | 0.67 | 0.00 | 6.13 | 3.24 | 32 % |
| yharnam-route | play-lockon | 9.04 | 1.43 | 2.67 | 1.78 | 0.83 | 1.47 | 0.01 | 0.01 | 0.83 | 0.91 | 0.59 | 0.00 | 5.20 | 2.71 | 32 % |
| yharnam-route | play-walk-again | 7.04 | 1.06 | 2.01 | 1.44 | 0.71 | 1.10 | 0.00 | 0.00 | 0.64 | 0.69 | 0.44 | 0.00 | 4.29 | 2.15 | 31 % |
| yharnam-fight | play-walk | 9.24 | 1.44 | 2.88 | 1.78 | 0.76 | 1.58 | 0.01 | 0.01 | 0.89 | 0.94 | 0.61 | 0.00 | 4.75 | 2.50 | 32 % |
| yharnam-fight | play-lockon | 7.47 | 1.14 | 2.14 | 1.56 | 0.72 | 1.15 | 0.00 | 0.00 | 0.66 | 0.81 | 0.48 | 0.00 | 4.31 | 2.29 | 32 % |
| yharnam-fight | play-fight-first | 5.82 | 0.85 | 1.56 | 1.29 | 0.66 | 0.85 | 0.02 | 0.00 | 0.47 | 0.67 | 0.35 | 0.00 | 3.36 | 1.80 | 32 % |
| yharnam-fight | play-heal | 6.81 | 1.03 | 1.91 | 1.42 | 0.74 | 1.08 | 0.01 | 0.01 | 0.60 | 0.72 | 0.44 | 0.00 | 3.58 | 1.83 | 32 % |

`pipeline`, `vertex` and `state` together are 31 to 33 % of `commands` in every segment. The write-tracking faults of the guest threads cost 4 to 6 ms a frame in Yharnam, plus 2.5 to 3.2 ms of `mprotect`. They run on the game's threads, not on the GPU thread, and that is what #809 targets.

**The GPU thread's `memmove` by caller at 120** (perf samples of one run per route, ms per present):

| route | segment | GPU thread (perf) | `memmove` | user data (`BindBuffers`) | index and vertex buffers | `Pipeline::BindResources` | PM4 processing | buffer cache sync | texture upload and detiling |
|---|---|---|---|---|---|---|---|---|---|
| yharnam-route | play-stand | 6.07 | 0.57 | 0.20 | 0.18 | 0.06 | 0.11 | 0.01 | 0.00 |
| yharnam-route | play-walk | 8.51 | 0.98 | 0.38 | 0.33 | 0.08 | 0.18 | 0.02 | 0.00 |
| yharnam-route | play-camera | 6.45 | 0.65 | 0.21 | 0.20 | 0.06 | 0.16 | 0.02 | 0.00 |
| yharnam-route | play-sprint | 7.80 | 0.80 | 0.28 | 0.23 | 0.10 | 0.16 | 0.02 | 0.00 |
| yharnam-route | play-dodge | 7.20 | 0.69 | 0.23 | 0.21 | 0.07 | 0.15 | 0.02 | 0.00 |
| yharnam-route | play-attack | 7.77 | 0.78 | 0.28 | 0.23 | 0.08 | 0.17 | 0.02 | 0.00 |
| yharnam-route | play-lockon | 6.39 | 0.62 | 0.21 | 0.15 | 0.07 | 0.15 | 0.02 | 0.00 |
| yharnam-route | play-walk-again | 4.88 | 0.42 | 0.13 | 0.11 | 0.04 | 0.13 | 0.01 | 0.00 |
| clinic-fight | play-stand | 6.79 | 0.70 | 0.32 | 0.14 | 0.07 | 0.15 | 0.02 | 0.00 |
| clinic-fight | play-walk | 5.28 | 0.57 | 0.23 | 0.12 | 0.06 | 0.14 | 0.01 | 0.00 |
| clinic-fight | play-camera | 4.45 | 0.46 | 0.15 | 0.10 | 0.05 | 0.14 | 0.02 | 0.00 |
| clinic-fight | play-dodge | 3.31 | 0.31 | 0.10 | 0.06 | 0.03 | 0.11 | 0.01 | 0.00 |
| clinic-fight | play-attack | 3.21 | 0.28 | 0.09 | 0.05 | 0.04 | 0.10 | 0.01 | 0.00 |
| clinic-fight | play-lockon | 2.94 | 0.28 | 0.09 | 0.06 | 0.03 | 0.10 | 0.01 | 0.00 |
| clinic-fight | play-walk-again | 3.68 | 0.36 | 0.12 | 0.07 | 0.04 | 0.11 | 0.01 | 0.00 |
| yharnam-fight | play-walk | 8.41 | 0.90 | 0.38 | 0.23 | 0.11 | 0.18 | 0.01 | 0.00 |
| yharnam-fight | play-lockon | 7.81 | 0.86 | 0.29 | 0.30 | 0.11 | 0.16 | 0.01 | 0.00 |
| yharnam-fight | play-fight-first | 5.96 | 0.63 | 0.21 | 0.17 | 0.06 | 0.15 | 0.03 | 0.00 |
| yharnam-fight | play-heal | 7.40 | 0.69 | 0.24 | 0.23 | 0.06 | 0.15 | 0.01 | 0.00 |
| yharnam-fight | play-fight-strong | 8.22 | 0.93 | 0.35 | 0.27 | 0.10 | 0.20 | 0.02 | 0.00 |
| yharnam-fight | play-heal-again | 9.45 | 0.96 | 0.38 | 0.29 | 0.08 | 0.16 | 0.04 | 0.00 |
| yharnam-fight | play-fight-last | 9.39 | 1.16 | 0.52 | 0.37 | 0.11 | 0.15 | 0.01 | 0.00 |
| yharnam-fight | play-sprint | 6.29 | 0.70 | 0.25 | 0.20 | 0.09 | 0.14 | 0.02 | 0.00 |

`memmove` is 0.3 to 1.2 ms per present, 9 to 12 % of the GPU thread. The flattened user data copy in `Rasterizer::BindBuffers` is the largest share (up to 0.52 ms in the fight), then index and vertex buffer binding (up to 0.37 ms). Syncs of CPU-modified pages left the GPU thread with the copy threads (#777) and are 0.02 ms. Texture uploads and detiling don't appear in these steady segments.

**What the numbers support.** The heaviest segments miss 8.33 ms at 120 with the GPU thread as the bound, after #776 and #777, and `pipeline`, `vertex` and `state` are above 15 % of its time. That meets #780's condition to start. For #779, the split of the heaviest segments' `commands` between its two stages is about 2.5 to 2.8 ms for stage A (PM4 processing outside the draws, plus `pipeline`) and 6.8 to 7.4 ms for stage B (the rest of `draw`: `bind`, `render`, `vertex`, `state`). Stage B alone would sit just under the 8.33 ms slot, so #779's assessment needs #780's result first.

### GPU time per frame in Yharnam (#827)

Measured on 2026-11-24 to find what the GPU spends a heavy frame on, after #779 found the GPU bounding `yharnam-route` `play-walk` at 120. `relwithdebinfo` build of `main` at `fafd4393` with a local probe (not committed) that writes a Vulkan timestamp at the end of every render pass, at every change of command kind or caller outside render passes (buffer cache uploads, copy-shader HLE copies, game dispatches, tiling, detiling, texture uploads), and at the start of every command buffer. Each part's time is its timestamp minus the one before it in GPU order, or minus the start of its command buffer when the GPU sat idle in between, so the parts add up to the GPU's busy time. `--window`, warm cache, 120 FPS, three runs with the probe and three without. The GPU's busy share and clock (`hwmon` `freq1_input`) were sampled every 0.1 s beside them. Load from other programs stayed under 1.5 cores for most of the runs, with bursts of 4–10 cores for up to a minute. The GPU times barely depend on it; the FPS do.

**The probe's sum matches the GPU's busy time.** In `play-walk` the timestamps add up to 8.59, 8.91 and 8.65 ms per frame. The busy share times the frame interval gives 8.79, 9.27 and 8.79 ms, 2–4 % more. In `play-stand`, which holds 120, the GPU needs 7.21–7.30 ms per present with the probe and 7.15–7.34 ms without, so the probe's cost is within the run-to-run spread.

**`yharnam-route` `play-walk` at 120**, ms per frame (three probe runs and their median; 97–110 FPS, GPU busy 90–96 % at 2335–2557 MHz):

| part | runs | median | share | per frame |
|---|---|---|---|---|
| G-buffer pass (1920x1080, 6 colour targets + depth and stencil) | 1.98, 2.10, 2.06 | 2.06 | 24 % | ~700 draws in 16 parts |
| copy-shader HLE: source uploads (buffer cache, staging to arena) | 1.88, 1.87, 1.90 | 1.88 | 22 % | ~20 uploads, 7.8–7.9 MB |
| copy-shader HLE: the copies | 0.86, 0.89, 0.85 | 0.86 | 10 % | 55 copies, ~5250 regions, 1.27 MB |
| full-screen passes, 1920x1080, one colour target, no depth | 0.61, 0.63, 0.60 | 0.61 | 7 % | 22 passes |
| 1920x1080, 2 colour targets + depth | 0.54, 0.55, 0.53 | 0.54 | 6 % | ~370 draws |
| shadow map, 4096x4096 | 0.51, 0.52, 0.51 | 0.51 | 6 % | ~285 draws in 8 parts |
| half resolution (960x540) passes | 0.47, 0.51, 0.50 | 0.50 | 6 % | ~210 draws |
| other render passes (small targets, mip chains) | 0.50, 0.51, 0.49 | 0.50 | 6 % | ~85 passes |
| 1920x1080, one colour target + depth | 0.38, 0.40, 0.37 | 0.38 | 4 % | ~120 draws in ~49 passes |
| game compute dispatches | 0.29, 0.29, 0.28 | 0.29 | 3 % | ~47 |
| image downloads: tiling and copy to a buffer | 0.20, 0.21, 0.20 | 0.20 | 2 % | 3 images |
| buffer cache uploads for draws | 0.16, 0.18, 0.14 | 0.16 | 2 % | ~7 uploads, 0.5–0.7 MB |
| texture uploads and detiling | 0.14, 0.15, 0.13 | 0.14 | 2 % | ~4 textures |
| other copies, clears and fills | 0.08, 0.08, 0.07 | 0.08 | 1 % | |
| **sum** | 8.59, 8.91, 8.65 | 8.65 | | |

Per frame: about 210 render passes, 230 barriers and 1860 draws. A part's time includes the barrier before it. `play-sprint` (8.07–8.30 ms per frame) and `play-dodge` (6.86–9.00) have the same mix; the HLE's two parts are 2.4 and 2.1 ms there (one run each). In `play-stand` (6.91–7.02) the HLE's copies and all buffer uploads together take 2.0 ms.

**What is our cost.** The game's own passes and dispatches take about 5.3 ms of the 8.65. Our translation adds about 3.3 ms (38 %):
- **The copy-shader HLE, 2.7 ms (32 %).** Each batch of the HLE (`ExecuteCopyShaderHLE`) obtains the whole merged source span, up to 64 MB, and the buffer cache uploads every CPU-written page in it: 7.8–7.9 MB a frame at about 4 GB/s, for 1.27 MB that is then copied. The copies run as 5250 regions of about 240 bytes each, at about 1.5 GB/s in device-local memory. #676 found the span upload cheap on the GPU thread; on the GPU it is the largest part of the frame after the G-buffer pass. #832 cut it to about 0.7 ms (below).
- **Image downloads, 0.20 ms.** Three images are tiled and copied to guest memory every frame.
- **Buffer uploads for draws, 0.16 ms, and texture uploads, 0.14 ms.**
- **Render pass splits.** Every copy outside a render pass ends the pass it falls into: the G-buffer pass runs in about 16 parts a frame, most of them split by the HLE's copies. Their cost is inside the parts above and is not measured apart.

Each of the small parts is 2 % of the frame or less, so they get no issue of their own. The game's passes were not compared against the console: there is no PS4 timing to compare with, so whether a translated shader is slower than it has to be stays open. The G-buffer pass alone takes 2.1 ms for about 700 draws.

**What the numbers support.** With #832 the GPU would need about 6.4 ms per present in `play-walk`, below the GPU thread's 8.0–8.9 ms of CPU (#779), so the GPU thread would bound the heaviest segments again. Until then the GPU bounds them, a third of it through the HLE.

### Copy shader HLE from staging (#832)

Measured on 2026-11-24 after #827 found a third of the GPU's frame in `play-walk` in the copy shader HLE. `relwithdebinfo`, `yharnam-route` at 120, `--window`, warm cache, three runs with the staged copies (the default) and three with `--no-copy-shader-staging` (today's path before #832), alternating; load from other programs 0.4 cores on average, at most 2.7. One run each with #827's timestamp probe.

**The sources never hold GPU-written data.** A probe counted every HLE call over the whole route: about 14,000 copies in 6,500 calls a second in the play segments, and not one source range on a page the GPU modified. `--check-copy-shader-staging` compared 14.0 GB of source bytes the arena held after today's sync with guest memory over the route: no byte differed, and no call fell back to today's path.

| segment | FPS staged | FPS today | GPU ms/frame staged (probe) | today (probe) | GPU thread CPU ms/present staged | today |
|---|---|---|---|---|---|---|
| `play-walk` | 116.3, 118.2, 118.2 | 111.3, 110.8, 110.9 | 6.53 | 8.68 | 7.54, 7.40, 7.40 | 7.97, 7.93, 7.98 |
| `play-sprint` | 119.7, 119.4, 119.6 | 118.4, 119.3, 119.0 | 6.37 | 8.30 | 6.95, 6.89, 6.91 | 7.04, 6.30, 6.89 |
| `play-dodge` | 119.7, 119.6, 119.8 | 119.8, 119.7, 119.7 | 6.40 | 8.45 | 7.59, 5.40, 5.59 | 5.03, 5.12, 4.98 |

In `play-walk` the probe's parts went from 2.06 ms of buffer uploads and 0.85 ms of HLE copies to 0.35 and 0.53 ms: the sources are no longer uploaded, the destinations' uploads remain, and the copies run as about 530 regions a frame from staging instead of 5250 from the arena. The GPU's busy share fell from 97–98 % to 81 % at a lower clock, so the GPU no longer bounds these segments. The GPU thread's system time fell by 0.25 ms per present in `play-walk`, from fewer write faults on the source pages, which keep their tracking. `play-dodge` held 120 on both paths; there the GPU thread took 0.3–0.5 ms more per present staged in two runs and 2.5 ms more in the third, against a tight 5.0–5.1 ms today. Its cause is not known; it stays below the frame interval.

`play-walk` misses 120 now by its long frames (p99 10–13 ms staged, 17 ms today), with neither the GPU (81 % busy) nor the GPU thread (7.4–7.5 ms of CPU per present) saturated on average.

## Distributions (#755)

`run bb-run --container IMAGE --bb-run PACKAGE/bb-run` plays a portable package inside a container
of another distribution: its user space, the host's kernel and GPU. The container gets what
distrobox shares with the host (the home folder, the runtime folder with the Wayland, PipeWire and
PulseAudio sockets, X11, `/dev/dri`, the host's network and IPC, your own user id) and the game and
the package folder read-only. It replaces bwrap, which rootless podman can't run inside, and stays
in the run's scope (`--cgroups=disabled`), so the memory limit, the samplers and the stop reach
`bb-run` in it. Distrobox itself is not used: its first start installs about 60 packages, the Vulkan
drivers, fontconfig and the X11 libraries among them, which would hide what the package needs. The
image is the distribution's official one with only what the README names installed (the driver and
the libraries a desktop brings, `--no-install-recommends`). Bazzite's image links `/home` to `var/home` but has no `/var/home`, so
its image adds that folder and nothing else.

`run reference --container IMAGE` runs the reference the same way, with the unpacked AppImage in
place of the package folder (#793). It also gets `/dev/input` and `/run/udev` read-only, because
the reference reads the rig's uinput pad through SDL. It starts in the run's `work/`, as on the
host: started anywhere else, the reference doesn't find `work/user` and takes the shadPS4 folder in
the shared home, with whatever saves are there.

Each image ran `main-menu` with `SDL_AUDIO_DRIVER=pulseaudio` (the logos and the title with sound)
and `new-game-to-clinic` with `SDL_AUDIO_DRIVER=pipewire` (a new game to first control in the
clinic), on the package of #737 (`v0.1.0-17-gd8170ae9`). Sound counted when `pw-dump` showed a running
output stream of `bb-run` with the forced driver's API during the run (`pipewire-pulse` for
PulseAudio, none for PipeWire's own).

| Image | Mesa (RADV) | Logos and title, PulseAudio | Clinic, PipeWire | Clinic shot vs host | Peak maps (title / clinic) |
|---|---|---|---|---|---|
| Fedora 44 (host) | 26.2.6 | – | yes | 0 | – / 1642 |
| Ubuntu 24.04 | 25.2.8 | yes | yes | 0.30 | 881 / 1876 |
| Ubuntu 22.04 | 23.2.1 | yes | yes | 0.46 | 874 / 1862 |
| Debian 13 | 25.0.7 | yes | no: GPU ring timeout in the intro after the character editor, 3 of 3 runs; fixed by #793, see below | – | 832 / – |
| Debian 13, Mesa from `trixie-backports` | 26.1.6 | yes | yes | 0.63 | 813 / 1866 |
| Debian 12 | 22.3.6 | yes | yes | 0.42 | 843 / 1844 |
| Arch Linux | 26.3.1 | yes | yes | 0.28 | 570 / 1532 |
| Bazzite (stable) | 26.2.7 | yes | yes | 0.41 | 537 / 1579 |

Debian 13's Mesa 25.0.7 hung because RADV before 25.1 gives sparse binding a queue family of its own and drops sparse binds sent to the graphics queue, where the buffer cache binds its arenas (Mesa commit `f58630f07c3`, found by bisecting 25.0 to 25.1 on this route). The reference hangs the same way in the same container. `bb-run` now sets RADV's driconf option `radv_legacy_sparse_binding=true` unless the environment sets it, which keeps sparse binding on the graphics queue; with it the route reached the clinic on 25.0.7 (#793).

"Yes" means the scenario finished, its shots show the scenario (the title menu, the hunter in the
clinic) and the sound check above held. The shot difference is `compare.difference` of the clinic
shot against the host run's (mean absolute difference per channel, of 255); the title shots differ by
at most 0.02. Ubuntu 24.04 with
only the driver and the display libraries reached the title without sound, as the README says.

**Memory mappings.** The kernel refuses new mappings of a process above `vm.max_map_count`, 65530 by
default on older distributions (Fedora, Arch and Ubuntu 24.04 ship 1048576). The rig records the
most mappings one process of the target had (`peak_maps`, sampled once a second). The package's
`bb-run` on the host had at most 1642 on the clinic route and 1606 on the Central Yharnam route
(`yharnam-route`), and 1532 to 1876 in the containers (other libraries). That is more than 30 times
below the old default, so `bb-run` doesn't check the limit at start. A route that comes near 30000 should add the check.

## Load times (#655)

```
tools/rig/rig.py run bb-run load-continue --window --user-from USER --cache-home DIR
tools/rig/rig.py run reference load-death-travel --window --user-from analysis/rig/manual/yharnam-enemies/work/user
tools/rig/load_times.py RUN...
```

`load-continue.scn` goes from the title through Play Offline to Continue. `load-death-travel.scn` continues from the `yharnam-enemies` save, lets the enemies kill the hunter, then walks four steps to the lantern he wakes at and answers "Return to the Hunter's Dream?" with YES. Both take a picture every second through the loads. `load_times.py` reads the runs and prints each load and, per target, scenario and load, the median and the spread over the finished runs.

**A load ends at the first picture with the HUD.** The game hides the HUD on the title menus, the loading pages and YOU DIED, and shows it in the first picture of the world. The test is the stamina bar's green pixels in a fixed box (0.38 of the box in play, 0 without the HUD). A Continue load starts at the A press after which the game starts loading: both targets log a PlayGo call there (`scePlayGoSetInstallSpeed`, and bb-run its NID `4AAcTU9R3XM`), and the notices before Continue come in different numbers. A death starts at the first picture without the HUD, which is YOU DIED. A travel starts at the YES press. At one picture a second each end is known to ±0.5 s and is reported as the midpoint. The pictures come at fixed times after the press, so two runs whose loads differ by less than a picture gap get the same value.

Measured on 2026-11-19 on a quiet machine (load average under 3, no builds, no difftest), with three runs of each after one warm-up run: bb-run at `1b2afa9`, a warm `--cache-home` and no pipeline builds in the measured runs; the reference with its default configuration (no pipeline cache) and the warm Mesa shader cache of earlier runs. Both targets loaded the same saves: the clinic's from a reference `clinic-fight` run, `yharnam-play` (`yharnam-route`'s) and `yharnam-enemies`.

| load | from → to | bb-run | reference |
|---|---|---|---|
| Continue, clinic | press → HUD | 6.9 s (6.4–7.5), spread 0 | 6.9 s, spread 0 |
| Continue, Central Yharnam (`yharnam-play`) | press → HUD | 6.9 s, spread 0 | 6.9 s, spread 0 |
| Continue, Central Yharnam (`yharnam-enemies`) | press → HUD | 6.9 s, spread 0 | 6.9 s, spread 0 |
| Death, Central Yharnam | YOU DIED → HUD at the lantern | 18.0 s (±1.0), spread 0 | 18.0 s, spread 0 |
| Lantern travel, Central Yharnam → Hunter's Dream | YES → HUD in the Dream | 9.5 s (±0.5), spread 0 | 9.5 s, spread 0 |

Every run of either target landed in the same one-second slot, so no load of `bb-run` is longer than the reference's by more than its spread, and none needed a follow-up issue. The loads are as long as the game makes them, not as long as the work takes:

**`perf` profile of the longest load (the death, `--perf`, 18.0 s as without it).** CPU time per thread over the 18 s, from `threads.csv`: `Core.Res.PostProcess` 2.9 s, `game main` 2.6 s, `shadPS4:GpuComm` 2.6 s, `GXWorker` 1.8 s, Havok 0.6 s, FMOD 0.4 s; 12.7 s in all, 1.8 s of it system time. The parts asked for:
- **File reads:** `Core.Res.CacheableFileLoader`, the thread that opens and reads the game's files, used 0.12 s of CPU, 0.08 s of it in the kernel.
- **Decompression:** `Core.Res.PostProcess` runs the guest's own decompression of the `.dcx` files: 25 % of the `cpu-clock:u` samples, nearly all in one guest function around 0x2622354, 2.9 s of CPU.
- **The game's own threads:** `game main` is mostly the frame pacer and the present (`flipper_wait_frame`, `gettimeofday` in the vDSO). The `GXWorker`s, Havok and FMOD keep running at their normal rate.
- **Shader and pipeline builds:** none. The log has no `Pipeline build` or `Pipeline wait` line in the run.
- **Texture uploads:** in `shadPS4:GpuComm`'s 2.6 s, which is the loading page's and YOU DIED's frames at the usual cost per frame. There is no burst.

Per second (from the death picture): seconds 0–4 are YOU DIED over the world, at the normal frame's CPU. The load's work falls in seconds 5–8, about 4 s with `Core.Res.PostProcess` at 25 to 65 % of a core. Seconds 9–15 are the loading page at about 30 % of one core in all, with no file or decompression work. In seconds 16–17 the area starts (another decompression burst), and the HUD is back at 18. So about 7 s of the 18 s is the loading page waiting with nothing to load, and the reference takes the same time. Shorter loads would mean changing what the game waits for there (#684), not faster file or decompression code.

### The death's loading hold (#684)

The wait is a minimum time the game gives the loading page of a death's reload, not work. MoveMapStep, the step that loads an area and enters it, sets a hold of 12 s when the load starts if the load's request asks for one, counts it down by the frame's step and closes the loading page only once the hold is over and the area is ready (addresses in `src/game/load/load_hold.hpp`). A gdb breakpoint on the hold's setter showed the request flag set on the death's reload only, and the page closing 12.0 s after the hold started; Continue and the lantern travel have no hold, so their 6.9 s and 9.5 s are the game's own work and fades.

`bb-run --load-hold SECONDS` (0 to 12, default 12, the game's) sets the hold. With `--load-hold 0` the page closes as soon as the area is ready. Measured on 2026-11-19 with `load-death-travel`, the same build with and without the option, interleaved, on a machine with other sessions' builds running (load average 3 to 7):

| load | default | `--load-hold 0` |
|---|---|---|
| Death to control at the lantern | 18.0 s (2 runs, spread 0) | 11.0 s (3 runs: 11.0, 11.0, 10.0) |
| Continue | 6.9 s (spread 0) | 6.9 s (8.0, 6.9, 6.9) |
| Lantern travel | 9.5 s (spread 0) | 9.5 s (spread 0) |

The death load is 7 s shorter. Continue and travel don't change (one Continue read a picture late on the loaded machine; the option doesn't touch that load). The first pictures of the area after the death load show the same scene with and without the option, with nothing appearing later.

The config key `load_hold` sets the same hold (#704), and the launcher's Advanced page writes it. Checked on 2026-11-21 with `load-death-travel`, a relwithdebinfo build given the key through `--config` (the rig's `--game-dir` and `--profile` win over the file), default and `load_hold = 0` interleaved, a warm cache: death to control 18.0 s and 17.0 s with the default, 10.0, 10.0 and 11.0 s with `load_hold = 0`; Continue 6.9 s and travel 9.5 s in every run. The log of each `load_hold = 0` run says `loading page hold 0 s`.

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

Every run also writes `gpu.csv`: ten times a second the rig reads amdgpu's `gpu_busy_percent` (`/sys/class/drm/card*/device/`, the busiest card when there are several), and `thread_cpu.py` adds a `GPU` line per segment, the mean busy share times the segment's span divided by its presents: GPU ms per present (#654). It is the whole GPU, so the desktop's own drawing counts in it (about 2 % on an idle desktop), and a GPU that is not amdgpu gets no `gpu.csv`. Together with the threads it says which side bounds a frame: a GPU share near 100 % at the target frame rate means the GPU, a busy share well under it with a thread near one core's frame slot means that thread. It is a share of time, not the frame's own GPU time, so a short segment with a load in it reads the load's work too.

Thread times and perf samples count CPU time, and load from other programs raises it for the same work. With 12 busy loops on the 16 hardware threads, the GPU thread's CPU per present rose 1.8 times, spread over all of its functions (#610, `docs/design/frame-rate.md`). So compare only runs made on a quiet machine. Hold the build lock exclusively during a measured run, and record `uptime` and the running compilers (`pgrep -a 'cc1plus|ld|ninja'`) before and after it.

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
| `buffer`, `sync`, `resident` | buffer cache: obtaining a buffer; copying CPU-modified guest memory into it (uploads of 64 KiB or more only hand their `memmove` to the two copy threads `shadPS4:GpuCopy`, #777); making sparse blocks resident |
| `texture`, `upload`, `detile`, `upload_new` | texture cache work of a draw; refreshing a dirty image from guest memory; detiling it; the share of `upload` that is the first upload of a cache image object (a new texture, or one evicted and created again; not a re-upload of a changed image) |
| `render`, `vertex`, `state` | render pass setup; vertex and index buffer binding; dynamic state and descriptor writes |
| `copy`, `irq`, `queue` | buffer copies and fills; interrupt handlers signalled on the GPU thread; callbacks queued to it |
| `fault` | write-tracking SIGSEGV handler, whole, on any guest thread |
| `inval_buffer`, `inval_texture` | the buffer and texture caches' share of a fault |
| `protect` | `mprotect` calls of write tracking (from a fault or from a cache sync) |
| `wait`, `submit`, `present` | the scheduler waiting for the GPU; end-of-frame housekeeping; the presenter thread's present call |
| `flip_wait` | a flip request waiting from its queueing to the vblank that took it |
| `replay` | replaying the draw scheduler's recorded Vulkan commands into its command buffer, on the recording thread (on the GPU thread with `--sync-recording`; `docs/design/command-recording.md`) |
| `reuse`, `lookup` | graphics pipeline lookups taken from the reuse table, and those that built the key from the registers (both inside `pipeline`, `docs/design/command-recording.md`, "Reusing pipeline lookups"); the calls of the two give the reuse rate |

The counters are for finding the big things. A scope reads the CPU's time-stamp counter twice and adds to counters of its own thread, about 20 ns in a tight loop, and `log_frame` collects the threads' counters once per frame and scales the ticks to ns against `CLOCK_MONOTONIC`. A `play-walk` frame of `yharnam-route` has about 47 000 scopes, so they add under 1 ms to it; the GPU thread's CPU time per frame with and without them differs by less than the spread between runs (#594). Counter numbers from runs before #594 read too high in proportion to a part's calls: a scope then cost about 120 ns. For the exact cost of a part use `perf` or `thread_cpu.py`. The counters need a time-stamp counter that runs at a constant rate on all cores (`constant_tsc` and `nonstop_tsc` in `/proc/cpuinfo`). A thread keeps its counters after it exits; past 256 threads that ever ran a scope, the rest add to shared atomics, slower and still counted. The fault part is the one that runs in a signal handler: it reads the time-stamp counter and adds to its thread's counters, with no lock and no allocation.

`tools/rig/frame_profile.py RUN [--segment GLOB]` reads a run's log, `events.csv` and `frames.csv` and prints for each scenario segment: the frame interval of the GPU thread (the submit-done to submit-done time), the slowest frames with their biggest parts (`build` is the `Pipeline build` and `Pipeline wait` time overlapping the frame), the parts' extra cost in a slow frame over a normal one, a normal frame, and the late presents (a present interval over 1.5 times the median): the time from the submit-done before the present to the present, and that frame's `commands`. Read it with `--cache-home` of an earlier run: a cold cache puts shader and pipeline builds into the slow frames and hides the rest.

### What profiling decided

- **Holes of direct memory are not read (#531).** A buffer sync of a range the game never wrote read the holes of the direct memory memfd page by page, and each read faulted in and cleared a page. `MemoryManager::CopySparseMemory` writes zeros for the holes of copies of 64 KiB or more without reading them (`lseek` `SEEK_DATA`).
- **The staging ring keeps its first 12 blocks (#531).** After the loading screens it had freed its blocks, and a later upload burst paid for new 16 MiB blocks and their first-touch faults in the middle of a frame.
- **Vertex syncs stay whole (#531).** Copying only the vertices a draw fetches saved about 4 ms in the one upload frame of the Yharnam walk but cost about 7 ms in every other frame, because each draw then scanned its indices again over pages no draw had synced. Not kept.
- **Texture bursts are new content (#532).** The upload bursts of the walk are images the cache sees for the first time, as the game streams an area in. They have to be uploaded, so nothing is deferred. `frame_profile.py` lists frames with over 50 refreshes and how many were first uploads.
- **The work semaphore is polled only when an operation waits for it (#589).** Every draw queried the timeline semaphore with an `ioctl`, about 2 to 3 ms of system time per frame. #590, #591 and #593 cut other per-draw costs found in the same profile (#533), and #592 skips rebuilding the shader specialization of an unchanged draw.
- **Reads of a buffer are only appended until a write asks about them (#593).** After each draw the runtime recorded every bound buffer for its barriers: a linear search for the buffer's ranges and an insert into a sorted interval list, about 0.8 ms of the GPU thread per `play-walk` frame, though almost no write ever checks those reads. A buffer now carries its place in the tracker, and reads are merged into the sorted list only when a write that checks for hazards overlaps their bounds. The barriers are the same; a unit test checks the answers against the old tracking. The GPU thread's CPU per present fell by about 0.5 ms in every play segment of `yharnam-route`.
- **`game main` busy-waited in the frame pacer**, about half of all CPU samples. It was not on the frame's critical path, but it cost a core. Since #656 the pacer sleeps until 2 ms before the end of the wait and spins only the rest (`docs/design/frame-rate.md`). `game main` went from 16 ms of CPU per frame at 60 and 32.5 ms at 30 to 4.3 to 6.4 ms at both rates in the play segments of `clinic-fight` and `yharnam-route`. The frame times stayed within the spread of the busy wait.
- **A gap once a minute comes from outside the game (#792).** A program that reads `/proc/PID/smaps_rollup` of every process (on the reference machine a desktop app, every 60 s) makes the kernel walk the page tables of all of the game's mappings, about 4 GiB resident, for about 70 ms while it holds the process's mmap lock for reading. Write tracking's `mprotect` needs the lock for writing, so the GPU thread's `sync` and every faulting guest thread wait for the walk: one frame of 50 to 100 ms, with `sync` and `protect` far above their neighbours. A 60-line program with a 3 GiB memfd and three threads calling `mprotect` shows the same stall, so it is not the game's doing. Cutting the mappings into 4 MiB VMAs did not help: the kernel hands the lock to a waiting writer between VMAs, but each handoff costs about 10 ms and the GPU thread makes about 4000 `mprotect` calls per frame. The stall goes away only if write tracking stops taking the lock for writing (userfaultfd write-protect, #809). Until then, drop or mark frames at those 60 s marks when comparing runs, or close the program that scans.
