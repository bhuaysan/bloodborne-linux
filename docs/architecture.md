# Architecture

## Overview

```
 ┌──────────────────────────────────────────────────────────┐
 │                      game process (Linux)                │
 │                                                          │
 │   original game code  ◄──hooks──►  decompiled C++        │
 │   (mapped from eboot)              (replaces functions)  │
 │          │  imports (by NID)                             │
 │          ├──────────────►  bundled PRXs (game's own)     │
 │          │                 libc.prx · libSceFios2.prx    │
 │          ▼                        │                      │
 │   HLE system libraries (ours)  ◄──┘                      │
 │   kernel · file I/O · system · video · GNM · net · …     │
 │          │                                               │
 │          ▼                                               │
 │   Linux: pthreads · mmap · files · SDL/evdev · Vulkan    │
 └──────────────────────────────────────────────────────────┘
```

## Components

The interfaces between the runtime parts (loader, import stubs, hooks, differential harness) are fixed in [design/runtime.md](design/runtime.md). How the runtime brings up the OS layer (bundled PRXs, TLS, threads, memory, files, runtime directories) is in [design/m3-core-os.md](design/m3-core-os.md).

### Executable extraction
`eboot.bin` is a SELF container. The dump used here stores its segments unencrypted and uncompressed (a "fake-signed" SELF), so a plain ELF can be rebuilt from the container. `tools/self2elf` rebuilds it into a local analysis directory, never into the repo (format in `docs/formats/self.md`).

### Loader
- Maps the ELF `PT_LOAD` segments into one reserved range at a chosen base (the eboot links at 0).
- Parses the PS4-specific dynamic section: imported libraries, modules and symbol NIDs.
- Applies relocations and fills the import tables with our implementations, or with logging stubs.
- Loads the game's bundled `libc.prx` and `libSceFios2.prx` next to the eboot, as one load set.

Starting the game is split over three parts (M3, see [design/m3-core-os.md](design/m3-core-os.md)):
- `bb::hle::link` registers the exports of the bundled PRXs, relocates every module against them and the HLE layer, and registers the PRXs' TLS templates. The eboot's own TLS block is a fixed reservation at the end of the runtime executable's static TLS (`bbeboottls`), so game code and our code share one thread pointer.
- `bb::hle::run_prx_init`, on the game's main thread, checks that thread's eboot TLS block, hands the eboot's process parameters (`PT_SCE_PROCPARAM`) to libkernel and runs the PRXs' initialisers.
- `bb-run` (`src/tools/bb_run.cpp`) does the rest: it maps the load set and calls `link`, applies the hooks, creates the game's main thread with its stack, and on that thread calls `run_prx_init` and then the eboot's entry point.

### HLE system libraries
Each imported Sony library becomes a module in our runtime. Every function is identified by its NID. Unimplemented functions resolve to a stub that logs the call, which shows us what to implement next. Two of the game's bundled PRX files, `libc.prx` and `libSceFios2.prx`, are needed at startup and are loaded the same way as the eboot (or later replaced by our code). The other five bundled PRXs are camera-image libraries that the game most likely never loads (see `docs/formats/imports.md`).

The PS4 OS is FreeBSD-based and uses the System V x86-64 ABI, the same as Linux. That makes most calls a direct translation. TLS layout and signal and exception behavior need care.

### Graphics
GNM builds AMD GCN command buffers (PM4 packets). The plan:
1. Parse the command buffers the game submits.
2. Track GPU state and resources (buffers, textures, render targets).
3. Translate GCN shader binaries to SPIR-V with our own translator.
4. Record and submit the equivalent Vulkan work.

The translator, its offline tests and the shader extraction are designed in [design/graphics.md](design/graphics.md).

### Function replacement
Decompiled functions are compiled into the runtime. At startup, a table of `{original_address, replacement}` pairs is applied. Each original function entry is patched to jump to the replacement. Replacements call other game functions through their original addresses. Only functions that serve playability, performance or quality are replaced, and the rest of the game keeps running as original machine code.

### Differential test harness
Decompiled functions are verified before the game boots. A test process maps the original executable through the loader. It then calls the original function and the C++ replacement with the same inputs, in the same initial memory state, and compares return values and touched memory. With the eboot alone (`EbootTest`), imports without an HLE implementation go to stubs whose calls are recorded and compared. Functions that need the OS layer, such as those that allocate, run with the full load set (`LoadSetTest`): the libkernel HLE modules and the game's own bundled PRXs (`libc.prx`, whose `malloc` the game uses, and `libSceFios2.prx`), with the game's heap and the thread's TLS in the snapshot. See [design/runtime.md](design/runtime.md#differential-test-harness).

### Symbols
Our analysis results (names, signatures, types, matched NIDs) live as text in `symbols/`. Scripts sync them with a local Ghidra project, so the project can be rebuilt at any time and the history is tracked in git.

### Analysis tooling
Python tools under `tools/` turn the dump into something Ghidra and `symbols/` can use. The tools that parse game files or files derived from them check the pinned hashes. `ghidra-sync` doesn't: it passes the ELF named with `--elf` to Ghidra unchecked. Generated output goes to `analysis/`. Only `nidmatch` and `ghidra-sync` update `symbols/`.

```
eboot.bin, sce_module/*.prx
   │ self2elf      SELF → ELF
   ▼
analysis/elf/*.elf ──elfimports──► module, library and import lists (analysis/imports/)
   │  │                 │
   │  │                 └─nidmatch (+ syscandidates, derive) ──► symbols/nids.csv
   │  └─noreturn──► non-returning imports (analysis/noreturn/)
   │ elf2std                         │ pre-script during the import
   ▼                                 ▼
standard ELF ──Ghidra import──► Ghidra project ◄──ghidra-sync──► symbols/functions.csv, symbols/types/
```

- `elf2std` rewrites the PS4 dynamic tables into standard ones and adds section headers for the unwind tables, so stock tools import the file.
- `noreturn` lists the imports that never return. A Ghidra pre-script marks them before auto-analysis (see `docs/formats/eh-frame.md`).
- `tools/common/` holds the shared code: the PS4 dynamic-table reader, the NID hash, the game and analysis paths, and the command-line helpers (output-directory guard, per-file loop).

## Non-goals
- CPU emulation or instruction translation.
- Byte-matching decompilation.
- A complete decompilation of the game.
- Supporting other games or other game versions.
