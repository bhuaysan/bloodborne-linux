# Source and license policy

Since 2026-11-06 the project builds on existing work: any public source may be read and used, and code may be reused under the license rules below. The only off-limits material is listed in [Off-limits](#off-limits).

## Off-limits

Never search for, open, fetch, read or run:

- **Leaked Sony SDK material:** headers, documentation, samples, tools and libraries from Sony's SDK, also when they are copied into another project. Before opening a file from a code source, check its path and license header. Skip directories and files named after the SDK, and Sony headers without a clear open-source license. A file that carries a Sony copyright notice or reads like SDK material is off-limits. Stop reading it and record it in the [exposure log](#exposure-log).
- **Copies of game data** other than the maintainer's own dump: no downloaded executables, assets, extracted param or table data, decrypted firmware modules or save files.
- **Third-party recompilation, decompilation and port projects** of Bloodborne or other FromSoftware games: their repositories and forks, and what comes out of them, such as symbol maps, function names, address lists, per-title patches (60 fps and the like) and cheat tables. Reverse engineering of the game itself is our own work. shadPS4 is a general emulator with no per-game code and stays allowed.

Everything else is allowed, with credit.

## Allowed, with credit

- Public documentation of every kind: PS4 platform and file formats, NID lists and databases, AMD GPU documentation, Vulkan and SPIR-V, ELF and ABI documents, FreeBSD and Linux man pages and headers, wikis, articles and talks.
- Source code and documentation of open-source projects, for example PS4 emulators (shadPS4 and others), Mesa, the Linux GPU drivers, LLVM and homebrew toolchains.
- Bloodborne and FromSoftware file formats: public format documentation, file-format libraries and tools, and param layouts and definitions. Dumps of the game's actual data (extracted params, assets, executables) count as game data, see above.
- Emulator compatibility notes and bug reports about the game.
- Tools run as programs, including emulators run on our game (for frames, logs and captured GPU command streams). shadPS4 run this way is our reference for what the game should do.

Facts from any of these sources can be used directly. Still check each fact against the game's own files or behavior before relying on it, and say in the code or doc where it was checked.

## Code reuse and licenses

The project is licensed under GPL-2.0-or-later ([LICENSE](../LICENSE)). Code from other projects may be copied or adapted when its license allows that in a GPL-2.0-or-later project.

- **Check the license of each file**, not only the project's. The file's own license header or SPDX tag decides. Without one, the nearest `LICENSE` or `COPYING` file in its directory or a parent directory of the same repository decides. Without either, the file counts as unlicensed. Unlicensed code gives facts only: write our own code from it.
- **The program as a whole is GPL-2.0-or-later**, and code is reused only when that stays true. Copy or adapt code under:
  - GPL-2.0-or-later, LGPL-2.1-only and LGPL-2.1-or-later;
  - the permissive licenses MIT (and X11 variants), BSD-2-Clause, BSD-3-Clause, ISC, zlib and BSL-1.0 (BSD-4-Clause, with its advertising clause, is not compatible);
  - MPL-2.0, in files of their own, when the file is not marked "Incompatible With Secondary Licenses".
- **Facts only, no copied code**, from GPL-2.0-only, GPL-3.0, LGPL-3.0, Apache-2.0 and AGPL code, non-commercial licenses (e.g. CC-BY-NC) and unlicensed files. GPL-2.0-only code would make the program GPL-2.0-only, and GPL-3.0, LGPL-3.0 and Apache-2.0 code would make it GPL-3.0; a GPL-2.0-only and a GPL-3.0/Apache-2.0 piece can't be combined at all. Reusing such code needs a maintainer decision that changes the line above first.
- **Whole files** go unchanged into `third_party/<project>/` together with their license text.
- **A file under several licenses** (`A OR B`) may be used under any allowed one of them; record which in the credit row. These rules cover code copied into the repository, not system libraries we link against (the Vulkan loader, glibc).
- **Adapted code** in our files keeps the original file's copyright and license notices unchanged in a header comment, and adds an SPDX tag of its license, a `// Source: <project> <path> @<commit>` line and a `// Changed for bloodborne-linux, <date>` line. Where the original header only refers to a license file, or the file has no header and the project's `LICENSE`/`COPYING` applies, that license file (with its copyright lines) goes into `third_party/<project>/` and the header points to it. GPL-2.0 §2(a) and LGPL-2.1 §2(b) require that notice of changes; we use it for every license.
- **Upstream preferred.** Take code from the project's own repository or release, and record the commit.
- **Credit.** Record every source in [references.md](references.md) in the same change that uses it. Code sources go into its code sources table: project and commit, license of the files, files or pages used, what was taken (facts or code), where it is used, and the date. One row per source and use, appended to that source's group. A bulk import from one commit is one row.

If a source's license or status is unclear, don't copy from it. Ask the maintainer.

## Deriving code from shadPS4

Since 2026-11-09 the renderer, the shader translator and the system libraries we are missing are derived from shadPS4 (GPL-2.0-or-later). The rules above apply to every file. Three more, because this is a lot of code:

- **One pinned release.** Bulk imports come from one tagged release, and its commit goes into [references.md](references.md). Later fixes come in one by one for an issue that needs them, each with its own commit in the `// Source:` line. We don't sync continuously: upstream changes these directories too often for a patch queue to last.
- **Check before importing.** Nearly every file in shadPS4's `src/` is tagged GPL-2.0-or-later, but not all of them are. Read `REUSE.toml` and the SPDX lines of what you import, and leave out what falls outside the rules above, such as the OFL fonts of its GUI.
- **Dependencies.** shadPS4's submodules fall under the same rules when copied. Link system packages for the ones that aren't on the allow-list (glslang, the Vulkan loader, OpenAL Soft) and leave out what this game doesn't need. FDK-AAC stays out, because its license isn't on the allow-list.

## Naming imported functions (NIDs)

PS4 executables import functions by NID, a short hash derived from the symbol name.

- Names may come from any allowed source: public NID lists and databases, emulator HLE code, our own candidates (standard C/POSIX/FreeBSD names, strings in the dump) and the derivation rules in `tools/nidmatch/`.
- Accept a name only if its NID matches an import, computed with the documented hashing scheme. Record it in `symbols/nids.csv` with its source.
- Unmatched NIDs keep a descriptive placeholder name based on observed behavior, e.g. `kernel_unk_<NID>`.

Commit a NID only together with what we know about it: a matched name in `symbols/nids.csv`, or a placeholder name and an implementation or stub in the runtime. The complete import and export lists are tool output from the game files (`tools/elfimports` writes them to `analysis/imports/`) and are never committed.

## Policy changes

| Date | Change |
|---|---|
| 2026-11-09 | Maintainer decision: third-party recompilation, decompilation and port projects of the game, and what comes out of them, are off-limits. Code is derived from shadPS4 in bulk under "Deriving code from shadPS4". |
| 2026-11-09 | File renamed from `clean-room.md`. The policy itself is unchanged. |
| 2026-11-06 | Maintainer decision: the clean-room approach is dropped. Every public source is allowed except leaked Sony SDK material and copies of game data. NID names may come from public NID lists and emulator code. The project moves from MIT to GPL-2.0-or-later, so GPL-2.0-or-later code (for example from PS4 emulators) may be reused with its notices. |
| 2026-11-05 | Maintainer decision: the four code source categories are allowed under license rules. Previously, emulator source and FromSoftware format material were off-limits, and driver source was not allowed. Modding material about the games stays off-limits but is now named in detail. NID name tables are off-limits from any source, and reading an emulator's HLE code for a library puts that library under the exposure-log handling. Tools run as programs are allowed, except tools from off-limits projects. AMD's Sea Islands documentation is named explicitly. |

## Exposure log

Leaked SDK material seen by accident goes here. Entries before 2026-11-06 recorded material that was off-limits under the earlier clean-room policy; they no longer restrict anything (their Handling column is superseded) and are kept as history.

| Date | What was seen | Handling |
|---|---|---|
| 2026-11-04 | Screenshots of psdevwiki tables pairing NIDs with function names for libSceRemoteplay, libSceSaveData, libSceRegMgr and libSceRtc | Superseded on 2026-11-06. Was: not copied or used, not even as test vectors. A name for a NID of these four libraries is accepted only together with an independent, recorded candidate source (a string in the dump, how the game uses the function, or public documentation that isn't a NID list and isn't a code source). |

## Game version

All addresses refer to one exact build:

| File | SHA-256 |
|---|---|
| `eboot.bin` (CUSA03173, patch 1.09) | `d65f0b4f01d59166aed16f8604196d8b7dd805abbf0758b356e8f1354c9429f9` |
| `sce_module/libc.prx` | `4378b47f46f1d856824a6f971db1a0c3f41833b77f49d2fba9538f667a139166` |
| `sce_module/libSceFace.prx` | `89e134d3162323b29ad89c4af77b9db6501acca4ea382a3d62284c5e99584bd5` |
| `sce_module/libSceFios2.prx` | `3b8eb4d5a5c5a7b381d8d0610077e6cd404c29695a686274802f6b2737f6bf22` |
| `sce_module/libSceHand.prx` | `0ef7de31c741b20a50b641bdc576ff9df467509eb511e60cb43d7ee5f240b070` |
| `sce_module/libSceHeadTracker.prx` | `2ebfb4c208c3fbdd38c5f7a0f95f50b682ce2126a539c9be3fb9792ad761e1cb` |
| `sce_module/libSceS3DConversion.prx` | `86a854022a1ad1a2940787acd7e47c7a84e67148b2b641dba5452771506d20b9` |
| `sce_module/libSceSmart.prx` | `43f433e5459033e201bea857002343bb2436c03c33401c4f0c938b3e197df1ab` |

The PRX files are the modules bundled with the same build. Tools must check these hashes and refuse to work on other builds. `tools/self2elf/self2elf.py` keeps a copy of this table.
