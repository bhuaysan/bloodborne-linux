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
- `candidates/`: hand-picked NID candidate names, one file per reason, one name per line. Give every name a `#` comment saying why it is a candidate. The file stem is the source label. The directory does not exist yet. It is created with its first file.

## Sync

`tools/ghidra-sync/ghidrasync.py` moves function names and signatures between a Ghidra project and `functions.csv`. It runs Ghidra headless through PyGhidra, which ships with Ghidra. `GHIDRA_INSTALL_DIR` comes from the environment or `local.env`. On first use `pyghidraRun` sets up its own Python environment from the wheels bundled with Ghidra (no network). Close the project in the Ghidra GUI first, because headless Ghidra needs the project lock.

```sh
# after an analysis session: merge our annotations into symbols/functions.csv
tools/ghidra-sync/ghidrasync.py export analysis/<project>/<name>.gpr

# apply symbols/ to an existing project (no analysis)
tools/ghidra-sync/ghidrasync.py import analysis/<project>/<name>.gpr

# build a fresh project from your own ELF, analyze it, then apply symbols/.
# Analyzing the eboot takes about an hour and 11 GiB, so never run two at once:
flock analysis/.ghidra-heavy.lock \
    tools/ghidra-sync/ghidrasync.py import analysis/<project>/<name>.gpr --elf analysis/elf-std/eboot.elf
```

`--program` picks the program inside the project (default `eboot.elf`). `--symbols` writes or reads another directory, e.g. for a trial run.

What it exports:

- Only functions whose name or signature we set (Ghidra's `USER_DEFINED` source). Ghidra's default names (`FUN_...`), thunks, external functions and names from the ELF's symbol tables stay out. They come back on every new analysis, so they don't need to be stored.
- The name without its namespace, and the signature as Ghidra prints it: return type, name, parameters, no calling convention. A signature set without a name keeps Ghidra's default name in the prototype (`FUN_<absolute address>`).
- Addresses relative to the program's image base, so a rebased import still matches.

How it merges:

- Rows are sorted by address, and the file is rewritten only when it changed.
- `status` and `file` are ours and never change. A new row starts as `named` (or `unknown` when only the signature is known).
- A value Ghidra has replaces the CSV's. An empty value keeps the CSV's, and rows Ghidra doesn't know stay. So export never deletes anything: to drop a name, edit the CSV.

Types (`types/*.h`) are not synced yet.
