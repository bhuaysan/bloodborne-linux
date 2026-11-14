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

A run writes `target.log`, `events.csv` (what the rig did, tied to places in the log), `frames.csv` (one line per present), `memory.csv` (`peak_program_bytes` in the summary leaves out the page cache of the game's files), `shots/*.png`, `kernel.log` and `summary.json`. For `bb-run`, `builds` and `segment_builds` in the summary spread the time its GPU thread spent on new pipelines (the `Pipeline build` and `Pipeline wait` lines in the log) over the frame intervals: the longest interval with a build in it and the most build time within one interval. The exit status is 0 when the scenario finished with no kernel GPU fault and an unchanged game directory. It is 1 after an out-of-memory kill, an unreadable kernel log, a target that exits before the scenario ends, a `shot` step without its picture, and a run with the layer in which the reference or a windowed `bb-run` presented no frame (a headless `bb-run` never presents).

Scenario steps are documented at the top of `tools/rig/scenario.py`, and scenarios live in `tools/rig/scenarios/`.

## Decisions

**A small Vulkan layer instead of MangoHud and desktop screenshots.** The layer (`tools/rig/layer/`) timestamps every `vkQueuePresentKHR` and copies the presented image on request. It works the same for any program that presents through Vulkan, so the reference and `bb-run` are measured in the same way once ours draws. It needs no window focus and takes a picture in milliseconds, where `spectacle` took up to 20 s and needed the active window. MangoHud is not used: it wrote no CSV in two runs, and picture conditions need the grabs anyway.

**Waits follow the game, not the clock.** Scenarios wait for log lines of the target (the game's file opens and system calls that shadPS4 logs) or for picture conditions on a small grey copy of the frame. Fixed sleeps remain only for menu input pacing. The log markers come from the reference, so a scenario for `bb-run` may need its own once our log differs.

**A captured frame is marked, not hidden.** A capture waits inside the present call, so `frames.csv` flags that frame and the statistics leave out the intervals around it.

**At most one picture a second.** With captures every 0.25 to 4 seconds the reference crashed around the end of the opening video in 4 of 13 runs (faults in guest memory and in emulator threads, no GPU fault). With few or no captures it was 0 of 16, and limited to one picture a second it was 5 of 5 clean. This is a correlation and the cause is not settled. A crash in the reference still ends a run with a non-zero exit status, so a failed run is visible.

**Fresh data directory per run, on purpose.** The reference uses `work/user/` inside the run directory (portable mode). A new game therefore never meets an old save. `--user-from` copies an earlier `user/` for runs that need one.

**A zenity stub answers dialogs.** On an empty data directory shadPS4 asks through SDL and zenity whether to migrate saves, and blocks until someone answers. The rig puts a stub first in `PATH` that answers "Do nothing" and logs the call to `zenity-calls.txt`. A stub that only fails makes SDL fall back to an X11 message box, which blocks in the same way.

**`bb-run` gets the same virtual gamepad as the reference.** The rig creates it once for the scenario, whichever target runs, and `bb-run`'s window turns SDL3's gamepad events into pad state as shadPS4's does (`src/shadps4_host/sdl_window.cpp`). Both targets get `SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS`, since the pad never belongs to the focused window. Both also get `SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT` for the virtual pad's vendor and product id: a real controller on the machine would be listed too, SDL's order between the two varies, and the game gives slot 0 to the first, so the taps got lost in most runs (#520). It gets the same limits, logging and checks. Its XDG directories are per run and outside the checkout, because it refuses runtime directories inside the source tree. So its shader and pipeline cache starts cold in every run; `--cache-home DIR` points its `XDG_CACHE_HOME` at a directory that stays, for a warm run after a cold one. With `--window` it presents through `video_core`, so the layer measures its frames and takes its pictures as it does the reference's.

## Matching the main menu

`main-menu.scn` runs on both targets and ends on the title menu (Play Online, Play Offline), the menu the game shows with no input. The main menu behind Play Offline needs the pad, which comes in R5. `rig.py compare` decides whether our menu matches the reference's, with the tolerance the maintainer set on 2026-11-11: two reference runs give the noise floor, and ours passes within twice that floor.

**The same frame is the menu once it stands still.** The scenario waits for the game's call that hides the splash screen (the reference logs it by name, `bb-run` by its NID), then until the picture has not changed by more than one grey level for 6 s. Only the title stays still that long; the logos fade and hold for less. It then takes 8 pictures 1.5 s apart.

**Several pictures per run, compared pair by pair.** The selected entry glows, and the glow pulses. Title pictures of nine earlier reference runs differed pairwise by 0 to 0.036, depending on whether they caught the glow in the same phase, so one pair gives a floor that can be zero by chance. The floor is the mean over all 64 pairs of a picture of the first reference run with one of the second. Ours is the mean over all pairs of one of our pictures with one of either reference run.

**Mean absolute difference over the full frame.** It is the mean of |ours − reference| over every channel of every pixel, 0 to 255. Everything that differs counts, in proportion to how much it differs, and there is no threshold to tune. A count of pixels over a threshold would hinge on that threshold and treat a slightly wrong colour over the whole frame like one wrong pixel. The scenario's picture conditions use the same measure on their small grey copy.

**Same resolution, no scaling.** Both programs present 1280×720, the default window of each. `compare` refuses pictures of different sizes rather than scaling them, because scaling would add a difference of its own.

**Time to the menu is when the picture stopped changing.** The stable wait logs how long the picture had been unchanged, so the time from the start of the target to the first menu frame (within the 6 s wait's tolerance, and the one picture a second it looks at) is in `events.csv`. Ours passes when it is at most twice the mean of the two reference runs.

Measured on 2026-11-11 (RADV, `relwithdebinfo`): the noise floor is 0.0259 and ours 0.0258, a ratio of 1.00. The reference reached the menu after 25.5 s and 26.3 s, `bb-run` after 20.0 s, a ratio of 0.77. Both run the menu at 30 fps with no stutter.

## Matching the main menu behind Play Offline

`main-menu-offline.scn` presses Down and A on the title and takes its pictures of the menu that follows (New Game, System, Log In). It checks that `bb-run`'s window feeds the virtual gamepad to the imported pad. Measured on 2026-11-11 (RADV, `relwithdebinfo`): noise floor 0.0263, ours 0.0262, a ratio of 1.00.

## Sound

`run --audio` starts the target with SDL's disk audio driver (`SDL_AUDIO_DRIVER=disk`), which writes what the program plays to a file in real time, in the format it opened (both targets open 48000 Hz, 8 channels, 32-bit float). The rig reads the file in 10 s windows (`tools/rig/audio.py`), puts the RMS, the peak, the silent windows and the RMS per window into `summary.json` under `audio` and deletes the file, which grows by 1.5 MB per second. The same span from the start of both targets is compared, because the logos and the title are driven by the game and not by input.

Measured on 2026-11-11 over the first 100 s from the start with no input (runs `20261111-191617-reference-none` and `20261111-191813-bb-run-none`, the same 10 windows of each): the reference RMS is 0.0188, ours 0.0184, a ratio of 0.98. The levels per 10 s window agree to within a few percent from the first music (0.0021, 0.0063, 0.0092, 0.0090, 0.0092, 0.0109, 0.0152, 0.0313, 0.0438 for the reference against 0.0022, 0.0064, 0.0092, 0.0090, 0.0092, 0.0110, 0.0153, 0.0318, 0.0415 for ours). That `bb-run` run ended with SIGKILL, before the SIGTERM fix of `InitHLELibs`; the run `20261111-192057-bb-run-none` (60 s, exit 124) shows that the fix lets the time limit end it. Since then the rig takes the run's time limit as the span. Ours logs `Opened audio device: Default Device (48000 Hz, 8 ch)` for the main and the BGM port and creates the AT9 instances (`sceAjmInstanceCreate ... At9Dec`), with no AJM or AudioOut error.

The disk driver replaces the device, so this measures what the game hands to SDL (AudioOut, AJM and ATRAC9), and not the device or PipeWire path.

## The name dialog of character creation

`new-game-name.scn` goes from the title through New Game and the opening video to the character editor, opens the name dialog (the game's ImeDialog), types one letter with the pad, presses Done and Finish, and ends once the game saves. It needs no code of ours beyond the imported ImeDialog, whose ImGui layer the imported presenter draws over the game frame, and the pad of `pad_input.cpp`: on 2026-11-11 `bb-run` (relwithdebinfo) and the reference both logged `ImeDialog OK text(len=1): "g"` and `GetResult -> endstatus=0`, and the editor shows the name afterwards. The debug preset stops earlier: UBSan aborts in LibAtrac9 (`utility.c:14`, shift by 32) when the opening video's audio starts (#469). After Finish the game saves and starts loading, but `bb-run` does not yet show the intro: its picture `13-intro` is black (run `20261111-230644-bb-run-new-game-name`, which the scenario ends one second after the save), while the reference shows the loading page. A longer earlier run (`20261111-230429-bb-run-new-game-name`) stops about 8 s into the loading on `Unimplemented PM4 type 0`, because the tessellation and geometry shader setup calls (`sceGnmSetLsShader`, `SetHsShader`, `SetEsShader`, `SetGsShader`) are still logging stubs. The hand-over to the intro is therefore not shown on `bb-run` and is left to #468. A `log` step matches only text logged after the previous `log` step's match, so the final save step cannot match the mounts at game start.

## The R5 acceptance run

`r5-acceptance.scn` goes from the title through New Game, the opening movie, the name dialog and the intro to the clinic, moves the character with the stick, exits to the title through Options, System and Exit Game (the game saves as it exits), and continues from the save back into the clinic. `r5-soak.scn` continues from that save (`--user-from` takes the `user/` directory the acceptance run leaves in its run directory, for `bb-run` as for the reference) and plays for 25 minutes from there: the stick back and forth, the camera and the lock-on button, a wait in each round for the picture to change after the stick moves (a frozen picture fails the run), and one picture a minute. The stick walks the character out of the starting room, so this is 25 minutes in the game after Continue, starting in the clinic. `r5-soak` ran on both targets, since its waits use the picture and log text the two share. `r5-acceptance` ran on `bb-run` only.

Measured on 2026-11-12 (RADV, `relwithdebinfo`, `--window`), `bb-run` in the run `20261112-011305-bb-run-r5-acceptance`, with `--audio`:

- The run finished its scenario in 321 s with no kernel GPU fault, no out-of-memory kill and an unchanged game directory. The opening movie plays (picture `05-movie`) with sound: the sound of the whole run has an RMS of 0.029 and a peak of 0.74, and its only silent 10 s window is the first one, before the first sound.
- The intro cutscene ends in the clinic (`10-clinic`) with the HUD, the stick moves the character (`11-moved`), the game saves on exit (two mounts of `SPRJ0005` in mode 0xa, right after the answer Yes), plays its logos again and shows the title. Continue loads the save into the clinic (`16-clinic-loaded`, with the HUD), and the stick moves it again (`17-moved-again`).
- Frame interval over the whole run: median 33.33 ms, p99 50.11 ms, longest 633 ms (18 intervals over twice the median, all in the intro and the loads). The six reference runs of `new-game-to-clinic` of 2026-11-09 give a median of 33.33 ms, a p99 of 49.9 to 50.1 ms and a longest of 483 to 517 ms. Peak program memory is 3.97 GB; the six reference runs give 4.07 to 4.08 GB. In the clinic itself both show a p99 of about 50 ms in a window of 540 frames (a few 50 ms intervals in 18 s).

The 25-minute run of `bb-run` (`20261112-014654-bb-run-r5-soak`) and of the reference with a save of its own from 2026-11-09 (`20261112-021319-reference-r5-soak`), both 1500 s of play after Continue:

| | `bb-run` | reference |
|---|---|---|
| frames in the 1500 s | 44012 | 44014 |
| median, p99 of the interval | 33.33 ms, 33.39 ms | 33.33 ms, 50.03 ms |
| longest interval | 66.7 ms | 50.2 ms |
| peak program memory | 3.50 GB | 3.87 GB |
| program memory, start to end | 3.4957 to 3.4967 GB | 3.8655 to 3.8658 GB |
| kernel GPU faults, crashes | none | none |

The kernel log of the `bb-run` run is empty (no `amdgpu` line). Both are held at 30 frames a second by the game's own frame pacing, so the interval says nothing about how much headroom there is.

**The scenarios wait on log lines and pictures, and not all dialogs come every time.** The notices after Play Offline ("Unable to connect to the network", "Starting game in offline mode") and the one about an unclean end of the last session appear in different numbers from run to run, and a press that meets none of them lands on a menu entry. The acceptance run takes the save mount that follows the last notice as the sign that the menu is up (`lognow` makes a `log` step ignore everything logged until then, since the mounts of earlier phases would match at once), and the soak presses A six times three seconds apart, which is harmless once the game is running. A loading page stands still like the title, so "loaded" is a still picture after a fixed 10 s for the page to go, and the shots show which one it was.

**`repeat COUNT` and `lognow` are scenario steps** (`tools/rig/scenario.py`). `repeat` writes the lines to the matching `end` out COUNT times and replaces `{i}` by the number, so a shot can be named per round. A mark inside it still can't be used twice.


## Frame times per segment (`rig.py perf`)

`perf SCENARIO --runs N` runs the scenario N times on `bb-run` of the checkout (the column `branch`), N times on the reference and, with `--main-bb-run BINARY`, N times on a `bb-run` built from `main` (the column `main`), one run at a time, then prints the report. Each target needs a save: `--bb-user` takes the `user/` directory of an `r5-acceptance` run and `--reference-user` that of a reference run (`work/user/`). Without `--runs` it reports on finished runs given with `--branch`, `--main` and `--reference`. A run that did not finish its scenario (its `events.csv` does not end with `end,scenario finished`) is left out with a message, since it gives no metric. `--only GLOB` limits the report to segments by name.

Per segment and column the report gives the median over the runs of the p50, p95, p99 and maximum frame interval and of the stutters (intervals over twice the segment's median), and the spread of the p95 and of the p99 (largest minus smallest of the runs). A column is flagged where its p95 or its p99 is above the reference's by more than the larger of the two spreads of that percentile (#528 added the p95: more than 5 % of the frames missing their 33.3 ms slot moves it to 50 ms, which the p99 rule does not see when both targets sit on the 50 ms step). A flagged segment gets its own issue in R8.2. The p99 of a 30 fps segment is 33.4 ms or 50 ms, since the game paces frames to the 33.33 ms grid and one late present moves a frame to the next 16.7 ms step, so the spread is 0 or a multiple of 16.7 ms.

`clinic-fight.scn` is the gameplay route: Continue from the save into the clinic, then segments named `play-*` of standing (20 s), walking, camera turns, dodging, attacking (`RB`, `R2`), lock-on with strafing, attacking and dodging, and walking again, about 220 s of play in all. The character has no weapon in the clinic, so the attack presses show the attack animation of an unarmed hunter at best; the dodge, the lock-on and the walk are real. The route is as long as R7 has opened for gameplay, and Central Yharnam replaces it once the game gets there.

Measured on 2026-11-14 (RADV, `relwithdebinfo`, `--window`, cold shader cache every run), five runs each, all finished with no kernel GPU fault. `bb-run` is `20261114-111713`, `-112225`, `-112738`, `-113250` and `-113803`; the reference is `20261114-114315`, `-114834`, `-115354`, `-115913` and `-120432` (all in `analysis/rig/runs/`, suffix `-bb-run-clinic-fight` and `-reference-clinic-fight`). A sixth, earlier `bb-run` run (`20261114-111141-bb-run-clinic-fight`) checked the scenario and is not in the table. No `main` column: the branch only changes tools, so its `bb-run` is the one of `main`.

| segment | | p50 | p95 | p99 (spread) | max | stutters |
|---|---|---|---|---|---|---|
| play-stand | `bb-run` | 33.33 | 33.38 | 33.41 (16.63) | 33.55 | 0 |
| | reference | 33.33 | 33.41 | 33.47 (16.73) | 49.97 | 0 |
| play-walk | `bb-run` | 33.33 | 33.38 | 49.99 (0.06) | 50.04 | 0 |
| | reference | 33.33 | 33.44 | 50.06 (50.16) | 50.09 | 0 |
| play-camera | `bb-run` | 33.33 | 33.38 | 33.48 (16.61) | 50.05 | 0 |
| | reference | 33.33 | 33.41 | 50.01 (48.76) | 50.19 | 0 |
| play-dodge | `bb-run` | 33.33 | 33.38 | 33.40 (16.64) | 50.05 | 0 |
| | reference | 33.33 | 49.97 | 50.05 (16.73) | 50.27 | 0 |
| play-attack | `bb-run` | 33.33 | 33.37 | 33.40 (16.66) | 50.04 | 0 |
| | reference | 33.33 | 33.39 | 33.43 (16.64) | 50.11 | 0 |
| play-lockon | `bb-run` | 33.33 | 33.38 | 33.43 (16.63) | 50.08 | 0 |
| | reference | 33.33 | 33.39 | 33.63 (16.65) | 50.12 | 0 |
| play-walk-again | `bb-run` | 33.33 | 33.37 | 33.40 (16.63) | 33.43 | 0 |
| | reference | 33.33 | 33.38 | 33.42 (16.63) | 50.00 | 0 |

No segment is flagged by the p99 rule: `bb-run` is at or below the reference's p99 everywhere, and neither has a stutter. The longest interval of a whole run is 500 ms on both (the loads before the route). The first two `bb-run` runs had a p99 of 50 ms in every segment, the next three 33.4 ms (except the walk, 50 ms in all five), the reference's first run likewise, so both targets show a run-to-run drift of the same size that the spread covers. The reference's second run (`20261114-114834`) is an outlier in `play-walk` (p50 35.98 ms, p99 83.63 ms, max 100 ms); it causes the 50.16 ms spread there, and the "not flagged" verdict for `play-walk` depends on it.

### Central Yharnam route (`yharnam-route.scn`)

`yharnam-route.scn` continues from a save at the great bonfire in Central Yharnam, made by playing the reference by hand (not committed, under `analysis/rig/manual/`). Both targets load it: `--user-from` (and `--bb-user`) takes the reference's `user/` directory (`home/ID/savedata/TITLE/SPRJ0005`) for `bb-run` too, and `rig.py` copies the save files into the layout `bb-run` uses (`savedata/SPRJ0005`). After Continue the picture shows the hunter by the carriage in Central Yharnam on both targets (`00-start`). The segments are `play-stand` (15 s), `play-walk` (out of the alcove into the street with the fire, under the arch), `play-camera`, `play-sprint` (`B` held 2.5 s, back and forth), `play-dodge`, `play-attack`, `play-lockon` and `play-walk-again`, about 235 s of play.

The route steers by pictures: the character starts in a cluttered alcove and the camera swings behind it as it moves, so one walk reaches the arch (the reference in the three runs viewed, `bb-run` in the first of three viewed) and another stops at the pyre by the fence (the other two `bb-run` runs viewed). In the pictures viewed the character never died or took a fight: no enemy is near on the way, so there is no lock-on target, and the lock-on press only re-centres the camera. A route that fights needs the way to the first enemies, which the hand-played save does not show.

**Scope:** this route covers the open street with fire and fog but not the enemy groups. `play-attack`, `play-lockon` and `play-dodge` measure swings and rolls in an empty street. #526 therefore stays open for the fighting route (it needs a save near the first enemies, or a route description from the maintainer); the commit refers to it with `Refs`, not `Fixes`.

Measured on 2026-11-14 (RADV, `relwithdebinfo`, `--window`, cold shader cache every run), five runs each, all finished with `end,scenario finished` and no kernel GPU fault. `bb-run` is `20261114-132516`, `-132959`, `-133444`, `-133929` and `-134414`; the reference is `20261114-134858`, `-135346`, `-135834`, `-140323` and `-140811` (in `analysis/rig/runs/`, suffix `-bb-run-yharnam-route` and `-reference-yharnam-route`). Two earlier reference runs (`20261114-131457`, `-132013`) checked the route and are not in the table.

| segment | | p50 | p95 | p99 (spread) | max | stutters |
|---|---|---|---|---|---|---|
| play-stand | `bb-run` | 33.33 | 50.02 | 50.16 (0.5) | 51.36 | 0 |
|  | reference | 33.33 | 33.39 | 50.06 (16.66) | 50.11 | 0 |
| play-walk | `bb-run` | 33.34 | 50.11 | 66.69 (16.94) | 83.34 | 4 |
|  | reference | 33.33 | 33.46 | 50.09 (0.06) | 100.0 | 2 |
| play-camera | `bb-run` | 33.34 | 50.08 | 51.46 (16.64) | 66.79 | 1 |
|  | reference | 33.33 | 33.43 | 50.07 (16.64) | 50.15 | 0 |
| play-sprint | `bb-run` | 33.35 | 50.09 | 65.0 (16.24) | 83.37 | 2 |
|  | reference | 33.33 | 33.44 | 50.05 (16.61) | 50.46 | 0 |
| play-dodge | `bb-run` | 33.34 | 50.06 | 51.03 (16.53) | 67.58 | 2 |
|  | reference | 33.33 | 33.41 | 49.92 (16.67) | 50.13 | 0 |
| play-attack | `bb-run` | 33.34 | 50.1 | 51.4 (16.12) | 83.36 | 3 |
|  | reference | 33.33 | 33.45 | 49.98 (16.65) | 50.19 | 0 |
| play-lockon | `bb-run` | 33.34 | 50.06 | 50.94 (16.24) | 83.35 | 3 |
|  | reference | 33.33 | 33.42 | 50.1 (0.08) | 50.24 | 0 |
| play-walk-again | `bb-run` | 33.34 | 50.04 | 50.29 (15.96) | 66.7 | 1 |
|  | reference | 33.33 | 33.43 | 50.1 (16.63) | 50.17 | 0 |

No segment is flagged: the p99 of `bb-run` is above the reference's by at most 16.7 ms (one step of the 16.7 ms grid), which is within the spread of the runs. The p95 is another matter. It is 33.4 ms for the reference in every segment and 50 ms for `bb-run`, including `play-stand`, where the hunter stands still: more than 5 % of the frames of `bb-run` in the street present one step late. `bb-run` also has stutters (intervals over twice the median) in every moving segment against none (two in `play-walk`) for the reference. The cold shader cache adds to `play-walk` (70 to 82 pipeline builds there, 250 to 270 ms in all, against 1 to 16 builds in the other segments).

The p95 gap and the stutters go into a follow-up issue in R8.2 (candidate for the profile of #513), together with the question whether the flag should compare p95 too.

**Cause and fix (#528).** The late presents were the flip grid, not slow frames (#513: a late present follows a GPU-thread frame with on-time `commands`, and its flip waited 14.7 ms for the vblank against 8.2 ms on average). The game paces its frames by its own clock, so its flips keep one phase against video out's 60 Hz vblank grid for seconds; where that phase sat within a frame's jitter of a vblank, a third of the flips missed it by a millisecond or two and were shown 16.7 ms late. `src/hle/video/vblank_grid.hpp` now gives a flip that is queued up to 4 ms after a vblank that vblank, and moves the grid after it (one period and 2 ms after the flip), so the next frames come before their vblank. The grid only moves later and a flip is never shown later than before, so no latency is added, and the 33.3 ms steps stay (a flip is still taken at every `flip_rate + 1`-th vblank).

Measured again on 2026-11-14 and 15 after the fix (RADV, `relwithdebinfo`, `--window`, warm shader cache for `bb-run` after one discarded warm-up run, `--user-from` the save above, runs alternating `bb-run` and reference, all finished with `end,scenario finished`). `bb-run` is `20261114-222159`, `-224055`, `-225845`, `-231117` and `-232717`; the reference is `20261114-223122`, `-225018`, `-230326`, `-231854` and `-233526`. Medians over the runs, p95 and p99 with their spreads; nothing is flagged by the p95 or the p99 rule:

| segment | | p95 (spread) | p99 (spread) | max | stutters |
|---|---|---|---|---|---|
| play-stand | `bb-run` | 33.35 (0.01) | 33.38 (0.03) | 33.45 | 0 |
|  | reference | 33.44 (16.65) | 50.07 (17.14) | 50.15 | 0 |
| play-walk | `bb-run` | 33.41 (2.02) | 36.45 (14.72) | 83.37 | 1 |
|  | reference | 50.04 (16.8) | 50.16 (33.41) | 100.15 | 3 |
| play-camera | `bb-run` | 33.73 (0.72) | 35.4 (0.01) | 36.29 | 0 |
|  | reference | 33.44 (16.66) | 50.04 (0.25) | 50.12 | 0 |
| play-sprint | `bb-run` | 33.38 (0.02) | 35.39 (0.04) | 36.13 | 0 |
|  | reference | 49.97 (16.65) | 50.05 (16.93) | 66.75 | 3 |
| play-dodge | `bb-run` | 33.37 (0.02) | 33.39 (0.34) | 35.43 | 0 |
|  | reference | 33.41 (16.65) | 50.04 (17.18) | 50.12 | 0 |
| play-attack | `bb-run` | 33.38 (18.7) | 35.4 (19.9) | 54.77 | 0 |
|  | reference | 49.95 (16.61) | 50.1 (17.16) | 66.69 | 1 |
| play-lockon | `bb-run` | 33.36 (18.74) | 33.4 (19.27) | 50.04 | 0 |
|  | reference | 33.42 (16.59) | 50.09 (0.17) | 50.15 | 0 |
| play-walk-again | `bb-run` | 33.38 (17.48) | 35.37 (20.3) | 37.09 | 0 |
|  | reference | 33.44 (16.71) | 50.1 (16.77) | 50.12 | 0 |

The p95 of `bb-run` is within the reference's in every `play-*` segment, and its stutters are 0 outside `play-walk` (reference 0 to 3). Before the fix the warm runs of #513 (`20261114-151347`, `-152257`, `-153241`, `-154434`, `-155217`, `-155953`, an earlier build) had a p99 of 50 ms in six of the eight segments; it is now 33 to 36 ms in seven. The reference itself sat on the 50 ms step in three segments in these runs, with the machine shared with other runs, so its p95 is no stricter than `bb-run`'s now. `play-walk` keeps its slow frames (#530, #531, #532).

Run against the cold-cache runs above (`20261114-132516` ... `-140811`), the new p95 rule of `rig.py perf` flags `play-walk`, `play-sprint`, `play-dodge`, `play-attack` and `play-lockon` of `bb-run` (p95 50 ms against 33.4 ms with a spread of 16.7 ms at most on the side that has one: `play-stand`, `play-camera` and `play-walk-again` have a spread of 16.7 on the `bb-run` side and are excused), the p99 rule none; on the runs after the fix it flags nothing.

### Central Yharnam fighting route (`yharnam-fight.scn`)

`yharnam-fight.scn` continues from a second save, made by playing the reference by hand from the great bonfire to just before a group of living enemies (not committed, `analysis/rig/manual/yharnam-enemies/work/user`; `--user-from` and `--bb-user` take it for both targets, as in the route above). After Continue the hunter stands under an arch with the street ahead and the enemies walk up while it stands, so the route starts at once. It walks out until they notice (`play-walk`), locks on and backs toward the arch (`play-lockon`), and fights in short exchanges of a swing, a roll, a swing and a roll to the side (`play-fight-first`, `play-fight-strong` with `R2`, `play-fight-last`), with a blood vial between them (`play-heal`, `play-heal-again`) and a sprint at the end (`play-sprint`). About 100 s of play.

The route is blind: it does not steer by the picture. The pictures of the ten runs below show the hunter alive in all of them (the echoes stay at 154, so no kill), the enemies in `01-enemies`, and a camera that often ends behind fences or walls in the later shots, so how long the enemies stay in contact differs between runs. In the fourth reference run (`20261114-183749`) the `Y` presses opened item descriptions (Beckoning Bell, Sinister Resonant Bell), so its heal and strong-attack segments were played in a menu; it is in the table as it finished.

Measured on 2026-11-14 (RADV, `relwithdebinfo`, `--window`, cold shader cache every run), five runs each, all finished with `end,scenario finished` and no kernel GPU fault. `bb-run` is `20261114-181407`, `-181703`, `-181957`, `-182253` and `-182549`; the reference is `20261114-182845`, `-183147`, `-183449`, `-183749` and `-184054` (in `analysis/rig/runs/`, suffix `-bb-run-yharnam-fight` and `-reference-yharnam-fight`).

| segment | | p50 | p95 | p99 (spread) | max | stutters |
|---|---|---|---|---|---|
| play-walk | `bb-run` | 33.33 | 33.4 | 50.03 (16.7) | 50.03 | 0 |
| | reference | 33.33 | 33.43 | 50.07 (0.24) | 50.07 | 0 |
| play-lockon | `bb-run` | 33.33 | 33.39 | 50.0 (0.08) | 50.03 | 0 |
| | reference | 33.33 | 33.39 | 50.01 (16.71) | 50.03 | 0 |
| play-fight-first | `bb-run` | 33.33 | 33.38 | 50.02 (0.06) | 50.05 | 0 |
| | reference | 33.33 | 33.38 | 50.09 (16.71) | 50.13 | 0 |
| play-heal | `bb-run` | 33.33 | 33.36 | 50.02 (0.04) | 50.03 | 0 |
| | reference | 33.33 | 33.39 | 33.42 (16.74) | 50.02 | 0 |
| play-fight-strong | `bb-run` | 33.33 | 50.0 | 50.03 (16.67) | 50.06 | 0 |
| | reference | 33.33 | 33.38 | 50.0 (16.77) | 50.13 | 0 |
| play-heal-again | `bb-run` | 33.33 | 33.36 | 50.01 (16.68) | 50.03 | 0 |
| | reference | 33.33 | 33.39 | 50.09 (16.77) | 50.14 | 0 |
| play-fight-last | `bb-run` | 33.33 | 33.36 | 33.39 (16.59) | 50.02 | 0 |
| | reference | 33.33 | 33.37 | 33.4 (16.71) | 50.04 | 0 |
| play-sprint | `bb-run` | 33.33 | 33.37 | 50.02 (16.61) | 50.04 | 0 |
| | reference | 33.33 | 33.37 | 50.0 (16.72) | 50.04 | 0 |

No segment is flagged: the p99 of `bb-run` is never above the reference's by more than the spread of the runs. Neither target has a stutter in the fight, and the maximum is 50 ms (one late present) on both. The one p95 difference is `play-fight-strong` (`bb-run` 50.0 ms, reference 33.38 ms). In the open street of the route above `bb-run` showed a p95 of 50 ms in every segment and stutters; here only that segment does, so the gap there is not specific to fighting. The fight shows no frame-time gap that #513 could profile; what separates the targets on this route is smaller than the 16.7 ms grid step.

## Profiling `bb-run` (#513)

Tracy is gone, so a slow frame is found with two tools: `perf` for where the threads spend CPU time, and frame counters for how much each part of the GPU thread's frame took.

### perf

```
tools/rig/rig.py run bb-run yharnam-route --window --user-from USER --perf --cache-home DIR
python3 tools/rig/perfmap.py --perf-data RUN/perf.data     # writes /tmp/perf-<pid>.map
perf report -i RUN/perf.data --no-children --sort comm,symbol -g none --stdio
```

`--perf` runs `bb-run` under `perf record -e cpu-clock:u -F 499 --call-graph fp --user-callchains` into the run's `perf.data`. With `kernel.perf_event_paranoid=2` (no sysctl change, no sudo) only user-space samples of our own process work, and the callchain event fails to open without `--user-callchains` ("Failure to open any events"). The `relwithdebinfo` preset builds with `BB_FRAME_POINTERS=ON` (`-fno-omit-frame-pointer`), so `--call-graph fp` unwinds our code and the system libraries it calls; guest code has no frame pointers to follow, so a chain stops at the first guest frame. Without a call chain, `--sort comm` is the split by thread (the guest's thread names, `shadPS4:GpuComm` for the GPU thread, `GXWorker:N`, `game main`).

`perfmap.py` writes the file perf reads for code outside any file: the guest functions of `symbols/functions.csv` at the eboot's load base (`loader::default_base`, 0x10000000, plus the link address). The file has only the functions named so far (a handful), so guest samples mostly show as raw addresses `0x1xxxxxxx`; subtract 0x10000000 for the link address in Ghidra. Samples are CPU time only: a thread that sleeps (a lock, a wait for the GPU) shows nothing, which is what the frame counters are for.

Run it under `flock` when other agents use the GPU, and keep a perf run apart from a counter run, so the counters do not show up in the samples.

### Frame counters

`bb-run --log profile=debug` (`rig.py run bb-run SCENARIO --log profile=debug`) turns on the counters of `src/common/frame_profile.hpp`; without it a disabled counter costs a few ns (one call and a branch). Once per frame (the guest's submit-done) the log gets one line:

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
| `texture`, `upload`, `detile` | texture cache work of a draw; refreshing a dirty image from guest memory; detiling it |
| `render`, `vertex`, `state` | render pass setup; vertex and index buffer binding; dynamic state and descriptor writes |
| `copy`, `irq`, `queue` | buffer copies and fills; interrupt handlers signalled on the GPU thread; callbacks queued to it |
| `fault` | write-tracking SIGSEGV handler, whole, on any guest thread |
| `inval_buffer`, `inval_texture` | the buffer and texture caches' share of a fault |
| `protect` | `mprotect` calls of write tracking (from a fault or from a cache sync) |
| `wait`, `submit`, `present` | the scheduler waiting for the GPU; end-of-frame housekeeping; the presenter thread's present call |
| `flip_wait` | a flip request waiting from its queueing to the vblank that took it |

A scope is two `clock_gettime` calls, about 40 ns, and a frame has 15 000 to 25 000 of them (`buffer` alone 10 000 to 17 000), so the counters add around 0.5 to 1 ms to a frame of the GPU thread: they are for finding the big things, not for exact small numbers. The fault part is the one that runs in a signal handler: it only reads the clock and adds to atomics.

`tools/rig/frame_profile.py RUN [--segment GLOB]` reads a run's log, `events.csv` and `frames.csv` and prints for each scenario segment: the frame interval of the GPU thread (the submit-done to submit-done time), the slowest frames with their biggest parts (`build` is the `Pipeline build` and `Pipeline wait` time overlapping the frame), the parts' extra cost in a slow frame over a normal one, a normal frame, and the late presents (a present interval over 1.5 times the median): the time from the submit-done before the present to the present, and that frame's `commands`. Read it with `--cache-home` of an earlier run: a cold cache puts shader and pipeline builds into the slow frames and hides the rest.

### What the profile of `yharnam-route` shows (2026-11-14)

RADV, `relwithdebinfo`, `--window`, a warm shader cache (`--cache-home`, filled by an earlier run), `--log profile=debug`. Runs (in `analysis/rig/runs/`, suffix `-bb-run-yharnam-route`): `20261114-155217` and `20261114-155953` carry the full set of counters, `20261114-151347`, `-152257` and `-154434` an earlier subset, `-150551` is the cold-cache run and `-153241` the `--perf` run. All finished with `end,scenario finished`.

**The worst segment of the GPU thread is `play-walk`.** Leaving out `logos` (loading and cold builds: GPU-thread frames of 350 to 433 ms in these runs, which is not the route), it has the longest GPU-thread frame interval of every run (94.8 to 103 ms) and three slow frames in each run, at the same places of the route: about 95, 59 and 56 to 58 ms against a median of 33.2 ms. The slow frames do show as present intervals of 67 to 100 ms, but only 2 to 3 of the 305 frames of `play-walk` are that long, so they raise its p99 (50.0 ms) and maximum (100.1 ms in `20261114-155217`, 83.3 in `-155953`; the three longest are 100.1, 66.7 and 50.3 ms, and 83.3, 66.7 and 66.7 ms) and not its p95 (33.5 and 33.4 ms), which is what #528 reports. 10 and 5 of the 302 presents (the tool leaves out the intervals next to a capture) are late (over 1.5 times the median). Per segment the GPU thread's frame interval has a p95 of 35 to 37 ms; the p95 of the present interval, which `rig.py perf` reports, is 33.4 or 50 ms depending on the segment and the run (`20261114-155217`: 50 ms in `stand`, `attack`, `lockon`, `continue` and `walk-again`, 33.4 to 33.5 ms elsewhere; the other runs put the 50 ms in other segments), so the 50 ms in every segment of #528 comes from where the frames fall against the 16.7 ms flip grid in a run, not from the content of a segment.

The three slow frames of `play-walk` (run `20261114-155953`, the other run matches within 3 ms; times in ms, summed over threads where it says so):

| frame | `commands` | `sync` | `vertex` | `upload` (images) | faults (thread time) | what it is |
|---|---|---|---|---|---|---|
| 94.8 | 83.7 | 30.9 | 34.8 | 11.5 (279) | 2715 (105.5) | buffer sync, texture upload, fault storm |
| 59.7 | 59.6 | 31.3 | 24.8 | 0 | 1270 (45.5) | buffer sync of about 31 ms |
| 55.6 | 31.2 | 1.4 | 3.0 | 0 | 2584 (93.1) | the GPU thread is not slow: guest threads stalled in faults |
| a normal frame | 23.8 | 1.3 | 2.7 | 0.1 (5) | 1204 (37) | |

- **Write-tracking faults** (#530). A normal frame of `play-walk` takes 1100 to 1250 write-tracking faults of 33 to 36 us each, 37 to 43 ms of thread time in all (`fault`), and 2200 to 2800 in the slow ones. 97 to 98 % of that is `inval_buffer` (the buffer cache marking the page CPU-modified), 8 to 9 ms is the `mprotect` calls (about 1250 of 7 us). The `perf` run shows the same: 27 % of the user samples of the `GXWorker:N` threads are inside the fault handler, 37 % of those in `pthread_mutex_lock` and 12 % in `mprotect`, so most of a fault is lock contention with the GPU thread, not work. A guest thread is stopped for all of it.
- **Buffer sync on the GPU thread** (#531). `sync` (copying CPU-modified guest memory into a buffer) is 1.3 ms in a normal frame and 29 to 31 ms in two of the three slow ones (4800 calls), almost all of it inside `vertex` (the vertex buffer binding). The same 31 ms in two runs points to a large range that the game rewrote at that point of the route.
- **Texture upload** (#532). 279 images refreshed in the 95 ms frame (11.5 ms, 0.7 ms of it detiling), against 5 in a normal frame.
- **The GPU thread is busy for 17 to 26 ms of a 33.3 ms frame** (`commands`; `draw` is 96 % of it): about 1400 to 2000 draws at 12 to 13 us (the per-draw cost of the GPU thread is #533, R8.3). Per normal `play-walk` frame: `bind` 7.5 to 8.5 ms (`buffer` 3.7 to 4.3, `texture` 2.3), `pipeline` lookup 2.3, `vertex` 2.7, `render` 1.7, `state` 0.9. In the perf samples of the GPU thread (21 % of all samples) the largest symbols are `TextureCache::FindImage` (1.6 % of all samples, 7 % of the thread), `memmove`, `radv_init_push_descriptor_set`, `XXH3` and `Rasterizer::ResetBindings`.
- **Late presents are mostly not slow frames.** Outside `play-walk`, the late presents (interval over 1.5 times the median) follow GPU thread frames with the `commands` time of an on-time frame (for example 23.0 against 22.7 ms in `play-attack`), and they come 2 to 8 ms later after the submit-done. In `play-walk` they follow frames with a longer `commands` time (38 to 44 ms against 24 to 26), but most of the 5 to 10 are not among the 2 to 3 slow ones. Over a whole run (`20261114-152257`) the flip request of a late present had waited 14.7 ms for its vblank against 8.2 ms on average (`flip_wait`). A frame whose request reaches `video_out` just after a vblank misses it and shows 16.7 ms later; with a margin this thin, a jitter of 1 to 2 ms is enough. The flip timing finding is posted on #528.
- **Pipeline builds** are not in these frames with a warm cache. In the cold run `20261114-150551` the slowest frames of the `logos` segment (416 and 227 ms) are 266 and 224 ms of build time.
- **`game main`, 48 % of all samples, is a busy wait** of the guest on `gettimeofday` (guest code at link addresses 0x2034af0 to 0x2034b6f), 43 % of it in the vDSO `clock_gettime`, with `posix_gettimeofday`, `observe_gettimeofday` and `MainLoopProbe::count` of ours another 3 % of all samples. It costs a core but is not on the frame's critical path.

