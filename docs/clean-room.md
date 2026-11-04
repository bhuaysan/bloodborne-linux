# Clean-room policy

This project is worked out independently. This file is the single source of truth for which references may be used.

## Off-limits

Never search for, open, fetch or read:

- Decompilation, recompilation or reverse-engineering/modding projects for Bloodborne or other FromSoftware games, including their wikis, symbol maps, struct or param dumps, and tool sources.
- Source code of PS4 emulators, PS4 recompilation or translation tools, and existing PS4 loader plugins for disassemblers.
- Leaked Sony SDK material: headers, documentation, samples, and symbol or NID name lists derived from it.

## Allowed, with credit

- Public documentation of PS4 platform formats (SELF, PKG, SFO, the NID hashing scheme).
- Hardware and specification documents: AMD GCN ISA manuals, Vulkan / SPIR-V, ELF, System V x86-64 ABI, FreeBSD and Linux man pages.
- General programming resources.

### Case by case

- **OpenOrbis PS4 toolchain** (homebrew SDK): only the format pages of its wiki, such as the ELF and dynlib data specification. Its source code, headers, stub libraries and any symbol or NID name tables are off-limits. Check what the wiki says against our own files before relying on it, and record which facts came from it.

Every source used is logged in [references.md](references.md) in the same change that uses it: source, what it was used for, and the date.

If you're unsure whether a source is allowed, don't open it. Ask the maintainer.

## Naming imported functions (NIDs)

PS4 executables import functions by NID, a short hash derived from the symbol name. They don't import by name. Public name lists mostly come from off-limits sources, so names are recovered like this:

1. Collect candidate names we can justify ourselves: standard C/POSIX/FreeBSD names, names suggested by how the game uses a function, and names appearing as strings in the dump.
2. Compute the NID of each candidate with the publicly documented hashing scheme.
3. Accept a name only if its NID matches an import. Record it in the NID table with where the candidate came from.
4. Unmatched NIDs keep a descriptive placeholder name based on observed behavior, e.g. `kernel_unk_<NID>`.

Commit a NID only together with what we know about it: a matched name in `symbols/nids.csv`, or a placeholder name and an implementation or stub in the runtime. The complete import and export lists are tool output from the game files (`tools/elfimports` writes them to `analysis/imports/`) and are never committed.

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
