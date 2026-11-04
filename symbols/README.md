# Symbols

Our own analysis results in text form: function names, signatures and types we worked out. These files are the source of truth. The local Ghidra project can always be rebuilt from your own `eboot.bin` plus these files.

They contain addresses and names we chose, never code or data from the game.

## Files

- `functions.csv`: one row per known function.
  - `address`: hex, relative to the executable's load base, e.g. `0x00412a30`
  - `name`: our descriptive name
  - `signature`: C-style prototype if known, otherwise empty
  - `status`: `unknown` | `named` | `decompiled` | `replaced`
  - `file`: the source file holding the C++ version, otherwise empty
- `types/`: struct, enum and typedef definitions as C headers, one header per subsystem.
- `nids.csv`: matched NIDs, one row per (NID, library). Written by `tools/nidmatch`, which hashes every row again on each run.
  - `nid`, `library`, `name`
  - `source`: where the candidate name came from, as `;`-separated labels (see `docs/formats/nid.md`)
- `candidates/`: hand-picked NID candidate names, one file per reason, one name per line. Give every name a `#` comment saying why it is a candidate. The file stem is the source label.

## Sync

Export and import scripts for Ghidra will be added in M1. Export after every analysis session and commit the result.
