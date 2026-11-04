# Imports of the pinned build

Which libraries the game imports, how many functions and data symbols from each, and which of them ship with the game. The record and symbol formats are in [elf-dynamic.md](elf-dynamic.md).

All numbers come from `tools/elfimports`, run on the 8 ELFs of the pinned build (`eboot.bin` and the 7 PRXs in `sce_module/`). Counts are unique NIDs. The tool writes the full lists, with NIDs, to `analysis/imports/`. They stay local: only NIDs that have a name or an implementation are committed (see the NID section of [clean-room.md](../clean-room.md)).

## Files

| File | Needed modules | Imported libraries | Imports | Exports |
|---|---|---|---|---|
| eboot | 42 | 43 | 681 func, 20 object | none |
| libc.prx | 2 | 2 | 77 func, 2 object | 1597 func, 539 object, 4 tls |
| libSceFios2.prx | 4 | 4 | 78 func, 3 object | 165 func |
| libSceFace.prx | 2 | 2 | 16 func, 2 object | 29 func |
| libSceHand.prx | 2 | 2 | 3 func, 2 object | 5 func |
| libSceHeadTracker.prx | 5 | 5 | 38 func, 2 object | 9 func |
| libSceS3DConversion.prx | 3 | 3 | 72 func, 2 object | 4 func |
| libSceSmart.prx | 2 | 2 | 54 func, 6 object | 48 func |

## Game-bundled or firmware

The eboot needs 42 modules (`DT_NEEDED`). Two of them ship with the game: **`libc.prx`** and **`libSceFios2.prx`**. `libc.prx` needs `libSceFios2.prx` too. These three files form the **static load set**, the code that is loaded at startup.

Every import of the static load set from `libc` or `libSceFios2` is exported by the bundled PRX. None are left over. The eboot uses 216 of libc's 2140 exports and 21 of libSceFios2's 165.

The other 40 modules come from the system firmware, and so do two more that only the bundled PRXs need: `libSceLibcInternal` and `libSceDiscMap` (both needed by libSceFios2).

The other 5 bundled PRXs (libSceFace, libSceHand, libSceHeadTracker, libSceS3DConversion, libSceSmart) are not needed by any file, and their names don't appear as strings in the eboot. Either the game loads them at runtime through a system call that takes a numeric module id, or it never loads them. Which of the two stays open until the import names are recovered by NID matching.

## Firmware imports of the static load set

539 NIDs in 39 libraries.

| Library | Module | NIDs | Types | Imported by |
|---|---|---|---|---|
| libkernel | libkernel | 133 | 131 func, 2 object | eboot, libSceFios2, libc |
| libSceGnmDriver | libSceGnmDriver | 50 | 50 func | eboot |
| libSceNet | libSceNet | 35 | 35 func | eboot |
| libScePosix | libkernel | 32 | 32 func | eboot |
| libSceNpMatching2 | libSceNpMatching2 | 27 | 27 func | eboot |
| libSceHttp | libSceHttp | 24 | 24 func | eboot |
| libSceNpManager | libSceNpManager | 20 | 20 func | eboot |
| libSceNpWebApi | libSceNpWebApi | 17 | 17 func | eboot |
| libSceAvPlayer | libSceAvPlayer | 16 | 16 func | eboot |
| libSceLibcInternal | libSceLibcInternal | 16 | 14 func, 2 object | libSceFios2 |
| libSceNpScore | libSceNpScore | 14 | 14 func | eboot |
| libSceAjm | libSceAjm | 12 | 12 func | eboot |
| libSceSaveData | libSceSaveData | 11 | 11 func | eboot |
| libSceVoice | libSceVoice | 11 | 11 func | eboot |
| libSceRtc | libSceRtc | 10 | 10 func | eboot |
| libScePad | libScePad | 9 | 9 func | eboot |
| libSceVideoOut | libSceVideoOut | 8 | 8 func | eboot |
| libSceNpSignaling | libSceNpSignaling | 7 | 7 func | eboot |
| libSceNpUtility | libSceNpUtility | 7 | 7 func | eboot |
| libSceAudioOut | libSceAudioOut | 6 | 6 func | eboot |
| libSceNetCtl | libSceNetCtl | 6 | 6 func | eboot |
| libSceNpTrophy | libSceNpTrophy | 6 | 6 func | eboot |
| libSceUserService | libSceUserService | 6 | 6 func | eboot |
| libSceImeDialog | libSceImeDialog | 5 | 5 func | eboot |
| libSceNpProfileDialog | libSceNpProfileDialog | 5 | 5 func | eboot |
| libScePlayGo | libScePlayGo | 5 | 5 func | eboot |
| libSceSystemService | libSceSystemService | 5 | 5 func | eboot |
| libSceMouse | libSceMouse | 4 | 4 func | eboot |
| libSceMsgDialog | libSceMsgDialog | 4 | 4 func | eboot |
| libSceNpAuth | libSceNpAuth | 4 | 4 func | eboot |
| libSceNpCommerce | libSceNpCommerce | 4 | 4 func | eboot |
| libSceSaveDataDialog | libSceSaveDataDialog | 4 | 4 func | eboot |
| libSceAppContent | libSceAppContentUtil | 3 | 3 func | eboot |
| libSceAudioIn | libSceAudioIn | 3 | 3 func | eboot |
| libSceSysmodule | libSceSysmodule | 3 | 3 func | eboot, libSceFios2 |
| libSceDiscMap | libSceDiscMap | 2 | 2 func | libSceFios2 |
| libSceNpCommon | libSceNpCommon | 2 | 2 func | eboot |
| libSceSsl | libSceSsl | 2 | 2 func | eboot |
| libSceCommonDialog | libSceCommonDialog | 1 | 1 func | eboot |

Grouped by area (our grouping, by library name):

| Area | Libraries | NIDs |
|---|---|---|
| Kernel and POSIX | libkernel, libScePosix | 165 |
| PSN | libSceNp* (11 libraries) | 113 |
| Network | libSceNet, libSceNetCtl, libSceHttp, libSceSsl | 67 |
| Graphics and display | libSceGnmDriver, libSceVideoOut | 58 |
| Audio | libSceAudioOut, libSceAudioIn, libSceAjm, libSceVoice | 32 |
| System services | libSceUserService, libSceSystemService, libScePlayGo, libSceAppContent, libSceRtc | 29 |
| Support for the bundled PRXs | libSceLibcInternal, libSceDiscMap, libSceSysmodule | 21 |
| Video playback | libSceAvPlayer | 16 |
| Save data | libSceSaveData, libSceSaveDataDialog | 15 |
| Input | libScePad, libSceMouse | 13 |
| Dialogs | libSceImeDialog, libSceMsgDialog, libSceCommonDialog | 10 |

## Declared but unused

The eboot needs 4 modules and declares their libraries without importing a single symbol from them: libSceGameLiveStreaming, libSceScreenShot, libSceVideoRecording and libSceWebBrowserDialog. libSceHeadTracker does the same with libSceCamera, libScePad and libSceSysmodule. The loader must accept a needed module that provides nothing.

## Firmware imports of the other bundled PRXs

Only relevant if the game loads them.

| PRX | Libraries (NIDs) |
|---|---|
| libSceFace | libSceLibcInternal (16), libkernel (2) |
| libSceHand | libSceLibcInternal (3), libkernel (2) |
| libSceHeadTracker | libSceLibcInternal (17), libkernel (23) |
| libSceS3DConversion | libSceGnmDriver (50), libSceLibcInternal (21), libkernel (3) |
| libSceSmart | libSceLibcInternal (53), libkernel (7) |
