# The missing system libraries (R5)

R5 makes the game playable: pad, sound with ATRAC9, the movies and the name dialog. shadPS4 v.0.21.0 (the pin of R3 and R4) has all of them. This note records how they come into our tree and the interfaces the R5 issues build on. How each piece works as built belongs in its comments and tests.

## What the game reaches

The reference run from the title to the clinic (`new-game-to-clinic`, 2026-11-09) logs calls into Pad, AudioOut, Ajm, AvPlayer and ImeDialog. It logs none into MsgDialog, SaveDataDialog or the Np dialogs. R5 takes the libraries the run reaches and nothing else:

| Game library | Upstream source | Notes |
|---|---|---|
| `libScePad` | `core/libraries/pad`, `input/controller` | fed from SDL3 gamepads |
| `libSceAudioOut` | `core/libraries/audio/audioout*`, `sdl_audio_out.cpp` | SDL3 backend only |
| `libSceAjm` | `core/libraries/ajm` | ATRAC9 through LibAtrac9, MP3 through FFmpeg, no AAC |
| `libSceAvPlayer` | `core/libraries/avplayer`, `common/support/avdec` | system FFmpeg |
| `libSceImeDialog`, `libSceCommonDialog` | `core/libraries/ime/ime_dialog*`, `system/commondialog` | drawn by the imported ImGui core |

## Decisions

**The libraries are imported whole and unchanged.** R4 adapted the GNM and VideoOut glue because those libraries sit on upstream's kernel. These five use little of it: a thread helper (`Kernel::Thread`), the process time, the SDK version, the mount points for file paths and a few settings. So the files come in through `tools/import-shadps4` like `video_core`, and the host layer provides that small surface. Fixes go into host headers or, where unavoidable, into a later adapted copy with its notice, never into the imported files. Since R6 that last rule is lifted: the imported files are edited in place, see [rewrite.md](rewrite.md).

**Registration through a host `libs.h`.** Each imported library has `RegisterLib(SymbolsResolver*)` full of `LIB_FUNCTION(nid, lib, ver, mod, fn)`. The host `core/libraries/libs.h` defines `LinkSymbolImpl` to add the entry to our NID table, and a host `InitHLELibs` calls the six `RegisterLib`s (CommonDialog with the five) before the game's imports are linked. It starts SDL's audio and gamepad subsystems first, as upstream's window does before its libraries register. A NID has exactly one owner: if an imported library and `src/hle` both register one, startup fails. Our stubs for these libraries (`src/hle/system/audio_out.cpp`) are removed in the same change. Names from these registrations go into `symbols/nids.csv` with shadPS4 as their source.

**Upstream's guest threads are ours.** `Kernel::Thread` starts a guest pthread, because callbacks such as AvPlayer's file streamer call guest code. The host `threads.h` maps `posix_pthread_create`, `_join` and the attribute calls onto `src/hle/kernel/thread.cpp`, so these threads get the guest TLS and signal stack like the game's own.

**Paths go through our kernel.** AvPlayer opens files by guest path (`/app0/movie/...`). The host `core/file_sys/fs.h` resolves `MntPoints::GetHostPath` with `src/hle/kernel/paths.cpp`, so the game directory stays read-only and behind one resolver.

**Our save data, user service and system service stay.** They carry the game today and have tests. Upstream's `savedata` is not imported. Imported files that include `userservice.h` or `systemservice.h` get host headers that declare what they use and forward to ours.

**SDL3 for sound and input.** SDL3 is already the window. Upstream's OpenAL backend and audio input are left out. The pad reads SDL3 gamepad events from the host window. The rig drives both programs with the same virtual gamepad.

**Codecs.** ATRAC9 comes from LibAtrac9 (MIT, copied unchanged into `third_party/`). Video, AAC in movies and MP3 go through the system FFmpeg (`libavcodec`, `libavformat`, `libswscale`, `libswresample` via pkg-config), as `docs/source-policy.md` asks for libraries off the allow-list. AJM's AAC decoder needs FDK-AAC, which stays out: `ajm_aac.cpp` is not imported and a host file makes AAC instances fail with upstream's error code. The reference run creates no AAC instance.

**Dialogs.** The ImeDialog draws through the imported ImGui core on our presenter and reads the pad through the imported controller, as upstream does. MsgDialog and SaveDataDialog stay stubs until a run reaches them; then they get an issue.
