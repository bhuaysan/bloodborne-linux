# Source and license policy

The file keeps its old name for links and tools. Since 2026-11-06 the project builds on existing work: any public source may be read and used, and code may be reused under the license rules below. The only off-limits material is listed in [Off-limits](#off-limits).

## Off-limits

Never search for, open, fetch, read or run:

- **Leaked Sony SDK material:** headers, documentation, samples, tools and libraries from Sony's SDK, also when they are copied into another project. Before opening a file from a code source, check its path and license header. Skip directories and files named after the SDK, and Sony headers without a clear open-source license. A file that carries a Sony copyright notice or reads like SDK material is off-limits. Stop reading it and record it in the [exposure log](#exposure-log).
- **Copies of game data** other than the maintainer's own dump: no downloaded executables, assets, decrypted firmware modules or save files.

Everything else is allowed, with credit.

## Allowed, with credit

- Public documentation of every kind: PS4 platform and file formats, NID lists and databases, AMD GPU documentation, Vulkan and SPIR-V, ELF and ABI documents, FreeBSD and Linux man pages and headers, wikis, articles and talks.
- Source code and documentation of open-source projects, for example PS4 emulators (shadPS4 and others), Mesa, the Linux GPU drivers, LLVM, homebrew toolchains, and PS4 recompilation or translation tools.
- Bloodborne and FromSoftware material: file-format libraries and tools, reverse-engineering and modding notes, decomp projects, symbol maps, param and struct dumps, address lists, cheat tables, per-title emulator patches and compatibility notes.
- Tools run as programs, including emulators run on our game (for frames, logs and captured GPU command streams).

Facts from any of these sources can be used directly. Still check each fact against the game's own files or behavior before relying on it, and say in the code or doc where it was checked.

## Code reuse and licenses

The project is licensed under GPL-2.0-or-later ([LICENSE](../LICENSE)). Code from other projects may be copied or adapted when its license allows that in a GPL-2.0-or-later project.

- **Check the license of each file**, not only the project's. The file's own license header or SPDX tag decides. Without one, the nearest `LICENSE` or `COPYING` file in its directory or a parent directory of the same repository decides. Without either, the file counts as unlicensed. Unlicensed code gives facts only: write our own code from it.
- **Compatible licenses:** GPL-2.0-only, GPL-2.0-or-later, LGPL-2.1 and later, and the permissive licenses (MIT and X11 variants, BSD-2-Clause, BSD-3-Clause, ISC, zlib, BSL-1.0). Apache-2.0, GPL-3.0 and LGPL-3.0 code may be used too, but it makes the combined program GPL-3.0-or-later; record that in the credit row. MPL-2.0 code stays in its own files. Code under AGPL, CC-BY-NC, other non-commercial licenses or no license is not copied.
- **Whole files** go unchanged into `third_party/<project>/` together with their license text (and Apache-2.0 `NOTICE` files).
- **Adapted code** in our files keeps the original copyright lines and an SPDX tag of its license in a header comment, plus `// Source: <project> <path> @<commit>`. Adapted Apache-2.0 files say that they were changed.
- **Upstream preferred.** Take code from the project's own repository or release, and record the commit.
- **Credit.** Record every source in [references.md](references.md) in the same change that uses it. Code sources go into its code sources table: project and commit, license of the files, files or pages used, what was taken (facts or code), where it is used, and the date.

If a source's license or status is unclear, don't copy from it. Ask the maintainer.

## Naming imported functions (NIDs)

PS4 executables import functions by NID, a short hash derived from the symbol name.

- Names may come from any allowed source: public NID lists and databases, emulator HLE code, our own candidates (standard C/POSIX/FreeBSD names, strings in the dump) and the derivation rules in `tools/nidmatch/`.
- Accept a name only if its NID matches an import, computed with the documented hashing scheme. Record it in `symbols/nids.csv` with its source.
- Unmatched NIDs keep a descriptive placeholder name based on observed behavior, e.g. `kernel_unk_<NID>`.

Commit a NID only together with what we know about it: a matched name in `symbols/nids.csv`, or a placeholder name and an implementation or stub in the runtime. The complete import and export lists are tool output from the game files (`tools/elfimports` writes them to `analysis/imports/`) and are never committed.

## Policy changes

| Date | Change |
|---|---|
| 2026-11-06 | Maintainer decision: the clean-room approach is dropped. Every public source is allowed except leaked Sony SDK material and copies of game data. NID names may come from public NID lists and emulator code. The project moves from MIT to GPL-2.0-or-later, so GPL code (for example from PS4 emulators) may be reused with its notices. |
| 2026-11-05 | Maintainer decision: the four code source categories are allowed under license rules. Previously, emulator source and FromSoftware format material were off-limits, and driver source was not allowed. Modding material about the games stays off-limits but is now named in detail. NID name tables are off-limits from any source, and reading an emulator's HLE code for a library puts that library under the exposure-log handling. Tools run as programs are allowed, except tools from off-limits projects. AMD's Sea Islands documentation is named explicitly. |

## Exposure log

Leaked SDK material seen by accident goes here. Entries before 2026-11-06 recorded material that was off-limits under the earlier clean-room policy; they no longer restrict anything and are kept as history.

| Date | What was seen | Handling |
|---|---|---|
| 2026-11-04 | Screenshots of psdevwiki tables pairing NIDs with function names for libSceRemoteplay, libSceSaveData, libSceRegMgr and libSceRtc | Not copied or used, not even as test vectors. A name for a NID of these four libraries is accepted only together with an independent, recorded candidate source (a string in the dump, how the game uses the function, or public documentation that isn't a NID list and isn't a code source). |

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
