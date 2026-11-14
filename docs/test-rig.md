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

Per segment and column the report gives the median over the runs of the p50, p95, p99 and maximum frame interval and of the stutters (intervals over twice the segment's median), and the spread of the p99 (largest minus smallest of the runs). A column is flagged where its p99 is above the reference's by more than the larger of the two spreads. A flagged segment gets its own issue in R8.2. The p99 of a 30 fps segment is 33.4 ms or 50 ms, since the game paces frames to the 33.33 ms grid and one late present moves a frame to the next 16.7 ms step, so the spread is 0 or a multiple of 16.7 ms.

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

No segment is flagged: `bb-run` is at or below the reference's p99 everywhere, and neither has a stutter. The longest interval of a whole run is 500 ms on both (the loads before the route). The first two `bb-run` runs had a p99 of 50 ms in every segment, the next three 33.4 ms (except the walk, 50 ms in all five), the reference's first run likewise, so both targets show a run-to-run drift of the same size that the spread covers.
