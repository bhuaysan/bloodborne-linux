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
```

A run writes `target.log`, `events.csv` (what the rig did, tied to places in the log), `frames.csv` (one line per present), `memory.csv` (`peak_program_bytes` in the summary leaves out the page cache of the game's files), `shots/*.png`, `kernel.log` and `summary.json`. The exit status is 0 when the scenario finished with no kernel GPU fault and an unchanged game directory.

Scenario steps are documented at the top of `tools/rig/scenario.py`, and scenarios live in `tools/rig/scenarios/`.

## Decisions

**A small Vulkan layer instead of MangoHud and desktop screenshots.** The layer (`tools/rig/layer/`) timestamps every `vkQueuePresentKHR` and copies the presented image on request. It works the same for any program that presents through Vulkan, so the reference and `bb-run` are measured in the same way once ours draws. It needs no window focus and takes a picture in milliseconds, where `spectacle` took up to 20 s and needed the active window. MangoHud is not used: it wrote no CSV in two runs, and picture conditions need the grabs anyway.

**Waits follow the game, not the clock.** Scenarios wait for log lines of the target (the game's file opens and system calls that shadPS4 logs) or for picture conditions on a small grey copy of the frame. Fixed sleeps remain only for menu input pacing. The log markers come from the reference, so a scenario for `bb-run` may need its own once our log differs.

**A captured frame is marked, not hidden.** A capture waits inside the present call, so `frames.csv` flags that frame and the statistics leave out the intervals around it.

**At most one picture a second.** With captures every 0.25 to 4 seconds the reference crashed around the end of the opening video in 4 of 13 runs (faults in guest memory and in emulator threads, no GPU fault). With few or no captures it was 0 of 16, and limited to one picture a second it was 5 of 5 clean. This is a correlation and the cause is not settled. A crash in the reference still ends a run with a non-zero exit status, so a failed run is visible.

**Fresh data directory per run, on purpose.** The reference uses `work/user/` inside the run directory (portable mode). A new game therefore never meets an old save. `--user-from` copies an earlier `user/` for runs that need one.

**A zenity stub answers dialogs.** On an empty data directory shadPS4 asks through SDL and zenity whether to migrate saves, and blocks until someone answers. The rig puts a stub first in `PATH` that answers "Do nothing" and logs the call to `zenity-calls.txt`. A stub that only fails makes SDL fall back to an X11 message box, which blocks in the same way.

**`bb-run` gets the same virtual gamepad as the reference.** The rig creates it once for the scenario, whichever target runs, and `bb-run`'s window turns SDL3's gamepad events into pad state as shadPS4's does (`src/shadps4_host/sdl_window.cpp`). Both targets get `SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS`, since the pad never belongs to the focused window. It gets the same limits, logging and checks. Its XDG directories are per run and outside the checkout, because it refuses runtime directories inside the source tree. With `--window` it presents through `video_core`, so the layer measures its frames and takes its pictures as it does the reference's.

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

`new-game-name.scn` goes from the title through New Game and the opening video to the character editor, opens the name dialog (the game's ImeDialog), types one letter with the pad, presses Done and Finish, and ends once the game saves. It needs no code of ours beyond the imported ImeDialog, whose ImGui layer the imported presenter draws over the game frame, and the pad of `pad_input.cpp`: on 2026-11-11 `bb-run` (relwithdebinfo) and the reference both logged `ImeDialog OK text(len=1): "g"` and `GetResult -> endstatus=0`, and the editor shows the name afterwards. The debug preset stops earlier: UBSan aborts in LibAtrac9 (`utility.c:14`, shift by 32) when the opening video's audio starts. After Finish `bb-run` still stops about 8 s into the loading that follows, on `Unimplemented PM4 type 0` (the tessellation and geometry shader setup calls are logging stubs), which is outside this scenario.
