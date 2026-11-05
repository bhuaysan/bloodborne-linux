# Clean-room policy

This project writes its own code, apart from credited third-party code reused under the rules below. This file is the single source of truth for which references may be used. Since 2026-11-05 some third-party source code may be read and, where its license allows, reused (see [Code sources](#code-sources)).

## Off-limits

Never search for, open, fetch, read or run:

- Leaked Sony SDK material: headers, documentation, samples, and symbol or NID name lists derived from it. This includes such material inside otherwise allowed sources, for example SDK headers or samples copied into an emulator. Before opening a file from a code source, check its path and license header: skip directories and files named after the SDK, or Sony headers without a clear project license. A file that carries a Sony copyright notice or reads like SDK material is off-limits: stop reading it and record it in the [exposure log](#exposure-log).
- NID name tables: any file or page whose purpose is to pair NIDs with symbol names, from any source (generated NID lists, NID databases, wiki tables). How the HLE code of an emulator is handled is described under [Code sources](#code-sources).
- Reverse-engineering, decompilation, recompilation and modding material for Bloodborne or other FromSoftware games, wherever it appears: projects, game-logic wikis, cheat tables, address lists, symbol maps, struct or param dumps, and per-title patches, workarounds and compatibility notes inside emulators. Only file-format material is allowed, under [Code sources](#code-sources).
- Standalone PS4 recompilation or translation projects (tools that translate PS4 executables ahead of time) and PS4 loader plugins for disassemblers: neither their source nor the tools themselves.

## Allowed, with credit

- Public documentation of PS4 platform formats (SELF, PKG, SFO, the NID hashing scheme).
- Hardware and specification documents: Vulkan / SPIR-V, ELF, System V x86-64 ABI, FreeBSD and Linux man pages. Encyclopedia and press articles about hardware, as weak background sources only.
- AMD's public GCN documentation for the PS4's GPU generation (GCN 2nd generation, "Sea Islands", gfx7): the *Sea Islands Series ISA Reference Guide*, the *Sea Islands 3D/Compute Register Reference Guide* and the *Southern Islands* programming guide (PM4 packets). The Southern Islands ISA and register guides may be used as cross-checks. The GCN3 (gfx8) and later ISA manuals may be read as background but never as the source of an encoding, because their encodings differ. Confirm encodings, register offsets and packet opcodes against the game's own shader binaries and command streams before relying on them. Record the document and revision in [references.md](references.md).
- FreeBSD's public system headers (BSD license), for constant values, `errno` numbers and struct layouts that the man pages name but don't give. Only from the upstream FreeBSD source tree or an installed FreeBSD system, never copies bundled with PS4 SDKs, toolchains or emulators. They are not a source of NID candidate names. Confirm each value against how the game or a bundled PRX uses it before relying on it, and record the FreeBSD release and the header files in [references.md](references.md).
- Tools run as programs, for example LLVM's AMDGPU disassembler as a test oracle for our shader decoder. Their output may be used. Reading a tool's source follows the rules for code sources. Tools from off-limits projects, and tools that bundle SDK material or NID name tables, may not be run.
- General programming resources.

### Case by case

- **OpenOrbis PS4 toolchain** (homebrew SDK): only the format pages of its wiki, such as the ELF and dynlib data specification. Its source code, headers, stub libraries and any symbol or NID name tables are off-limits. Check what the wiki says against our own files before relying on it, and record which facts came from it.

## Code sources

These categories of third-party source code and documentation may be read:

1. **AMD GPU code under permissive licenses:** AMD's addrlib and register database as found in Mesa (`src/amd/addrlib`, `src/amd/registers`), the AMD register and PM4 headers of the Linux `amdgpu` and `radeon` drivers, and AMD's UMR register database.
2. **Mesa's AMD driver and compiler code:** RADV, radeonsi, ACO, NIR and `src/amd/common`.
3. **FromSoftware file formats:** documentation and the source of file-format libraries and tools (for example DCX, BND, FLVER, TPF). Only the format material. Everything else about the games stays off-limits (see above).
4. **PS4 emulators:** their source code and documentation, including internal components such as the shader translator and the executable loader. Two parts need care:
   - **HLE library code** registers Sony functions by NID and name. It may be read for behavior. Reading the code of a library puts that library under the same handling as the libraries in the [exposure log](#exposure-log): record it there, and a name for one of its NIDs then needs an independent, recorded candidate source. Derived candidates are not accepted for it. Before reading the HLE code of a library that already has `derive-*` names in `symbols/nids.csv`, ask the maintainer. The change that adds a library to the exposure log also adds it to `NO_DERIVE_LIBRARIES` in `tools/nidmatch/nidmatch.py`. Names committed before the entry stay: the same change lists their NIDs as kept in `nidmatch.py`, so the tool doesn't refuse them.
   - **Running an emulator** on our game, to observe frames or capture GPU command streams, is allowed. Run it with function-call logging off. Its logs and debug output name Sony functions and count like its HLE code: every library named in logs you read goes into the exposure log.

Rules for every code source:

- **Upstream only.** Use the project's own repository or release, never a copy bundled with another project. The locations named in the categories above count as upstream.
- **Check the license of each file**, not only the project's. The file's own license header or SPDX tag decides. Without one, the nearest `LICENSE` or `COPYING` file in its directory or a parent directory of the same repository decides. Without either, the file counts as unlicensed.
- **Permissive licenses** (MIT and X11 variants, BSD-2-Clause, BSD-3-Clause, ISC, zlib, Apache-2.0, BSL-1.0): code may be copied or adapted. Copied files go unchanged into `third_party/<project>/` together with the license text that applies to them. Adapted code keeps the original copyright line and license text in a header comment of our file. For Apache-2.0, the project's `NOTICE`, if it has one, goes into `third_party/<project>/NOTICE` (for adapted code too, with the header comment pointing to it), and adapted files say that they were changed. Our own code stays MIT ([LICENSE](../LICENSE)).
- **A file under several licenses** counts as permissive if one of them is on the permissive list. Record which one is used.
- **All other licenses** (GPL, LGPL, AGPL, MPL, EPL, CC-BY and anything not on the permissive list) **and unlicensed files:** facts only, such as values, layouts, formats and observed behavior. Write our own code and docs in our own words and structure. Never copy code, comments or tables, and never translate code line by line.
- **Sony names:** don't adopt Sony API, type, struct or field names from these sources. Name things after what they do, or after AMD's documentation and register databases. NID names still come only from the hash matching below. Names in code sources are not candidates, because their origin is unclear.
- **Confirm against the game.** Check each fact against the game's own files or behavior before relying on it, and say in the code or doc where it was checked.
- **Credit.** Record every code source in the [code sources table](references.md#code-sources) of references.md, one row per project and release or commit: license of the files, files or pages consulted, what was taken (facts or code), where it is used, and the date. Later changes that use the same commit add to that row. Code or docs that use a fact from a code source say so in a comment, e.g. `// Source: <project> <path> @<commit> (<license>)`.

Every source used is logged in [references.md](references.md) in the same change that uses it: source, what it was used for, and the date.

If you're unsure whether a source is allowed, don't open it. Ask the maintainer.

## Naming imported functions (NIDs)

PS4 executables import functions by NID, a short hash derived from the symbol name. They don't import by name. Public name lists mostly come from off-limits sources, so names are recovered like this:

1. Collect candidate names we can justify ourselves: standard C/POSIX/FreeBSD names, names suggested by how the game uses a function, and names appearing as strings in the dump.
2. Compute the NID of each candidate with the publicly documented hashing scheme.
3. Accept a name only if its NID matches an import. Record it in the NID table with where the candidate came from.
4. Unmatched NIDs keep a descriptive placeholder name based on observed behavior, e.g. `kernel_unk_<NID>`.

### Derived candidates

Candidates may also be derived from names we already have, by rules. Example: our matches pair `pthread_attr_setstacksize` with `scePthreadAttrSetstacksize`, so a rule may turn other POSIX names into the same form. A rule must meet all of these:

- It is code in `tools/nidmatch/`, so anyone can run it again and get the same candidates.
- Its inputs are allowed sources only: standard C/POSIX/FreeBSD names (manual pages, system libraries), strings in the dump, and names already matched in `symbols/nids.csv`. Fixed words in the rule, such as a prefix, must appear in those matches or strings.
- It is motivated by our own matches, and its entry in [formats/nid.md](formats/nid.md) cites them.
- It has its own source label, so every name it produces can be traced back to it.
- It never uses the name prefixes of the libraries in the exposure log below. The same applies to libSceSaveDataDialog, whose names start with the libSceSaveData prefix. A derived name for one of these libraries is not accepted; it still needs an independent source.

A matching NID is still required, so a rule can only ever name real imports. Wrong guesses cost nothing.

Names that someone (person or agent) simply knows from elsewhere are **not** a candidate source, because that knowledge may come from off-limits material. A hand-picked name in `symbols/candidates/` needs a reason from the game itself (how it uses the function) or from allowed documentation other than the code sources and their documentation, written next to it.

Commit a NID only together with what we know about it: a matched name in `symbols/nids.csv`, or a placeholder name and an implementation or stub in the runtime. The complete import and export lists are tool output from the game files (`tools/elfimports` writes them to `analysis/imports/`) and are never committed.

## Policy changes

Changes from 2026-11-05 on.

| Date | Change |
|---|---|
| 2026-11-05 | Maintainer decision: the four [code source](#code-sources) categories are allowed under license rules. Previously, emulator source and FromSoftware format material were off-limits, and driver source was not allowed. Modding material about the games stays off-limits but is now named in detail. NID name tables are off-limits from any source, and reading an emulator's HLE code for a library puts that library under the exposure-log handling. Tools run as programs are allowed, except tools from off-limits projects. AMD's Sea Islands documentation is named explicitly. |

## Exposure log

Off-limits material that was seen by accident, and libraries whose emulator HLE code or logs were read ([Code sources](#code-sources), category 4), and how they are handled. Seeing them is recorded here, not hidden.

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
