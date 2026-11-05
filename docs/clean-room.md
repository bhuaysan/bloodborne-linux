# Clean-room policy

This project writes its own code. This file is the single source of truth for which references may be used. Since 2026-11-05 some third-party source code may be read and, where its license allows, reused (see [Code sources](#code-sources)).

## Off-limits

Never search for, open, fetch or read:

- Leaked Sony SDK material: headers, documentation, samples, and symbol or NID name lists derived from it. This includes such material inside otherwise allowed sources, for example SDK headers or samples copied into an emulator. A file that carries a Sony copyright notice or reads like SDK material is off-limits: stop reading it and record it in the [exposure log](#exposure-log).
- Decompilation or recompilation projects for Bloodborne or other FromSoftware games, and their symbol maps, struct or param dumps. Material about the games' file formats is allowed under [Code sources](#code-sources).
- Source code of PS4 recompilation or translation tools and of PS4 loader plugins for disassemblers.

## Allowed, with credit

- Public documentation of PS4 platform formats (SELF, PKG, SFO, the NID hashing scheme).
- Hardware and specification documents: Vulkan / SPIR-V, ELF, System V x86-64 ABI, FreeBSD and Linux man pages.
- AMD's public GCN documentation for the PS4's GPU generation (GCN 2nd generation, "Sea Islands", gfx7): the *Sea Islands Series ISA Reference Guide*, the *Sea Islands 3D/Compute Register Reference Guide* and the *Southern Islands* programming guide (PM4 packets). The Southern Islands ISA and register guides may be used as cross-checks. The GCN3 (gfx8) and later ISA manuals use different encodings and are not valid for decoding the game's shaders. Confirm encodings, register offsets and packet opcodes against the game's own shader binaries and command streams before relying on them. Record the document and revision in [references.md](references.md).
- FreeBSD's public system headers (BSD license), for constant values, `errno` numbers and struct layouts that the man pages name but don't give. Only from the upstream FreeBSD source tree or an installed FreeBSD system, never copies bundled with PS4 SDKs, toolchains or emulators. They are not a source of NID candidate names. Confirm each value against how the game or a bundled PRX uses it before relying on it, and record the FreeBSD release and the header files in [references.md](references.md).
- Tools run as programs, for example LLVM's AMDGPU disassembler as a test oracle for our shader decoder. Their output may be used. Reading a tool's source follows the rules for code sources.
- General programming resources.

### Case by case

- **OpenOrbis PS4 toolchain** (homebrew SDK): only the format pages of its wiki, such as the ELF and dynlib data specification. Its source code, headers, stub libraries and any symbol or NID name tables are off-limits. Check what the wiki says against our own files before relying on it, and record which facts came from it.

## Code sources

These categories of third-party source code may be read:

1. **AMD GPU code under permissive licenses:** Mesa's `src/amd/addrlib` and `src/amd/registers`, the AMD register and PM4 headers of the Linux `amdgpu` and `radeon` drivers, and AMD's UMR register database.
2. **Mesa's AMD driver and compiler code:** RADV, radeonsi, ACO, NIR and `src/amd/common`.
3. **FromSoftware file formats:** documentation and the source of file-format libraries and tools (for example DCX, BND, FLVER, TPF). Only the format material: game logic, symbol maps, struct and param dumps stay off-limits.
4. **PS4 emulators:** their source code and documentation.

Rules for every code source:

- **Upstream only.** Use the project's own repository or release, never a copy bundled with another project.
- **Check the license of each file**, not only the project's. The SPDX header or the license file next to it decides. A file without a clear license counts as unlicensed.
- **Permissive licenses** (MIT, BSD, ISC, zlib, Apache-2.0, BSL-1.0): code may be copied or adapted. Copied files go unchanged into `third_party/<project>/` with the project's license file. Adapted code keeps the original copyright line and license text in a header comment of our file. Apache-2.0 code also keeps the project's `NOTICE` text. Our own code stays MIT ([LICENSE](../LICENSE)).
- **Copyleft licenses** (GPL, LGPL, AGPL, MPL) **and unlicensed files:** facts only, such as values, layouts, formats and observed behavior. Write our own code and docs in our own words and structure. Never copy code, comments or tables, and never translate code line by line.
- **Sony names:** don't adopt Sony API, type, struct or field names from these sources. Name things after what they do, or after AMD's documentation. NID names still come only from the hash matching below. Names in code sources are not candidates, because their origin is unclear.
- **Confirm against the game.** Check each fact against the game's own files or behavior before relying on it, and say in the code or doc where it was checked.
- **Credit.** Record every code source in the code sources table of [references.md](references.md): project, release or commit, license of the files, files consulted, what was taken (facts or code), where it is used, and the date. Code or docs that use a fact from a code source say so in a comment, e.g. `// Source: <project> <path> @<commit> (<license>)`.

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

Names that someone (person or agent) simply knows from elsewhere are **not** a candidate source, because that knowledge may come from off-limits material. A hand-picked name in `symbols/candidates/` needs a reason from the game itself (how it uses the function) or from allowed documentation, written next to it.

Commit a NID only together with what we know about it: a matched name in `symbols/nids.csv`, or a placeholder name and an implementation or stub in the runtime. The complete import and export lists are tool output from the game files (`tools/elfimports` writes them to `analysis/imports/`) and are never committed.

## Policy changes

| Date | Change |
|---|---|
| 2026-11-05 | Maintainer decision: the four [code source](#code-sources) categories are allowed under license rules, and AMD's Sea Islands documentation is named explicitly. Previously, emulator source, FromSoftware format material and all driver source were off-limits. Leaked SDK material and NID name lists stay off-limits. |

## Exposure log

Off-limits material that was seen by accident, and how it is handled. Seeing it is recorded here, not hidden.

| Date | What was seen | Handling |
|---|---|---|
| 2026-11-04 | Screenshots of psdevwiki tables pairing NIDs with function names for libSceRemoteplay, libSceSaveData, libSceRegMgr and libSceRtc | Not copied or used, not even as test vectors. A name for a NID of these four libraries is accepted only together with an independent, recorded candidate source (a string in the dump, how the game uses the function, or public documentation that isn't a NID list). |

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
