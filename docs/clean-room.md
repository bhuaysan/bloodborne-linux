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

Every source used is logged in [references.md](references.md) in the same change that uses it: source, what it was used for, and the date.

If you're unsure whether a source is allowed, don't open it. Ask the maintainer.

## Naming imported functions (NIDs)

PS4 executables import functions by NID, a short hash derived from the symbol name. They don't import by name. Public name lists mostly come from off-limits sources, so names are recovered like this:

1. Collect candidate names we can justify ourselves: standard C/POSIX/FreeBSD names, names suggested by how the game uses a function, and names appearing as strings in the dump.
2. Compute the NID of each candidate with the publicly documented hashing scheme.
3. Accept a name only if its NID matches an import. Record it in the NID table with where the candidate came from.
4. Unmatched NIDs keep a descriptive placeholder name based on observed behavior, e.g. `kernel_unk_<NID>`.

## Game version

All addresses refer to one exact build:

| File | SHA-256 |
|---|---|
| `eboot.bin` (CUSA03173, patch 1.09) | `d65f0b4f01d59166aed16f8604196d8b7dd805abbf0758b356e8f1354c9429f9` |

Tools must check this hash and refuse to work on other builds.
