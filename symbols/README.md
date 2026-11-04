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
- `types/`: struct, union, enum and typedef definitions as C headers, one header `<subsystem>.h` per Ghidra category `/bb/<subsystem>`. Written by the sync tool (see "Sync"), in Ghidra's C dialect: field types use Ghidra's names (`uint`, `undefined4`), and every struct spells out its layout.
- `nids.csv`: matched NIDs, one row per (NID, library). Written by `tools/nidmatch`, which hashes every row again on each run.
  - `nid`, `library`, `name`
  - `source`: where the candidate name came from, as `;`-separated labels (see `docs/formats/nid.md`)
- `candidates/`: hand-picked NID candidate names, one file per reason, one name per line. Give every name a `#` comment saying why it is a candidate. The file stem is the source label. The directory does not exist yet. It is created with its first file.

## Sync

`tools/ghidra-sync/ghidrasync.py` moves function names and signatures between a Ghidra project and `functions.csv`, and our data types between the project and `types/`. It runs Ghidra headless through PyGhidra, which ships with Ghidra. `GHIDRA_INSTALL_DIR` comes from the environment or `local.env`. Close the project in the Ghidra GUI first, because headless Ghidra needs the project lock.

PyGhidra needs a one-time setup. The tool uses the `python3` it runs under when that has `pyghidra`, otherwise the virtual environment `pyghidraRun` keeps in Ghidra's settings directory (`$XDG_CONFIG_HOME/ghidra/ghidra_<version>_PUBLIC/venv`, default `~/.config`). It never installs anything itself, and the round-trip test skips until one of the two exists. To create the environment, run `$GHIDRA_INSTALL_DIR/support/pyghidraRun` once in a terminal and answer `y` to installing PyGhidra and to a new Ghidra virtual environment (on a Python that is externally managed it creates the environment without asking). Or create it by hand from the bundled packages:

```sh
venv=~/.config/ghidra/ghidra_<version>_PUBLIC/venv
python3.13 -m venv "$venv"
"$venv/bin/pip" install --no-index -f "$GHIDRA_INSTALL_DIR/Ghidra/Features/PyGhidra/pypkg/dist" pyghidra
```

Ghidra bundles JPype wheels only up to a certain Python version (cp313 for Ghidra 12.1). With a newer Python, pip builds JPype from its source archive, which needs a C++ compiler and the Python headers. An older supported Python avoids that.

```sh
# after an analysis session: merge our annotations into symbols/functions.csv and types/
tools/ghidra-sync/ghidrasync.py export analysis/<project>/<name>.gpr

# apply symbols/ to an existing project (no analysis)
tools/ghidra-sync/ghidrasync.py import analysis/<project>/<name>.gpr

# build a fresh project from your own ELF, analyze it, then apply symbols/.
# Analyzing the eboot takes about an hour and 11 GiB, so never run two at once:
flock analysis/.ghidra-heavy.lock \
    tools/ghidra-sync/ghidrasync.py import analysis/<project>/<name>.gpr --elf analysis/elf-std/eboot.elf
```

`--program` picks the program inside the project (default `eboot.elf`). `--symbols` writes or reads another directory, e.g. for a trial run.

### Functions

What it exports:

- Only functions whose name or signature we set (Ghidra's `USER_DEFINED` source). Ghidra's default names (`FUN_...`), thunks, external functions and names from the ELF's symbol tables stay out. They come back on every new analysis, so they don't need to be stored.
- The name without its namespace, and the signature as Ghidra prints it: return type, name, parameters, no calling convention. A signature set without a name keeps Ghidra's default name in the prototype (`FUN_<absolute address>`).
- Addresses relative to the program's image base, so a rebased import still matches.

How it merges:

- Rows are sorted by address, and the file is rewritten only when it changed.
- `status` and `file` are ours and never change. A new row starts as `named` (or `unknown` when only the signature is known).
- A value Ghidra has replaces the CSV's. An empty value keeps the CSV's, and rows Ghidra doesn't know stay. So export never deletes anything: to drop a name, edit the CSV.

### Types

Keep our types in Ghidra's Data Type Manager under `/bb/<subsystem>` (letters, digits, `_`), e.g. `/bb/game/Actor`. Nothing else goes out: Ghidra's built-in types, the archives it applies during analysis and types in other categories stay in the project.

Export writes one header per subsystem and rewrites it only when it changed. A header for a subsystem the project doesn't have stays as it is, so to drop a header, delete the file. Within a subsystem the header is the whole category: a type deleted in Ghidra disappears from it.

What a header looks like:

- `#include "<other>.h"` for each other subsystem it uses, then `#pragma pack(push, 1)` and forward declarations of its structs and unions.
- Types sorted by name, except that a type comes after the types it contains by value and after every typedef it names, through a pointer too.
- Enums with their values sorted by value, in decimal.
- Structs with an offset comment on every field. Every gap, including padding at the end, is a field `undefined1 _pad_0x<offset>[n]`. With packing 1 the layout follows from the header alone, whatever the alignment of the fields. The prefix `_pad_` is reserved for this.
- `_Static_assert(sizeof(...) == size, "name")` after every struct, union and enum.

Import parses the headers with Ghidra's C parser in include order, puts each type into `/bb/<subsystem>` and replaces a type of the same name there. It runs before the functions are applied, so signatures can use our types. It never deletes a type. Structs and unions come back unpacked, with their gaps as undefined bytes, the way the structure editor makes them. The sizes from `_Static_assert` are checked, and an enum gets its size and values from the header.

Not supported yet, export stops with an error: bit fields, function definitions (and so function pointers), pointers to arrays, pointers of a size other than 8, nested categories below `/bb/<subsystem>`, references to types outside `/bb`, the same type name in two subsystems (the headers share one C namespace), unions larger than their largest member (add a member that covers the whole union). Includes must not form a cycle, so two subsystems can't point at each other's types, not even through a pointer. Export checks all of this before it writes any header. Field comments and type descriptions are not exported.
