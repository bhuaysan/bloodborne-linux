# Architecture

## Overview

```
 ┌──────────────────────────────────────────────────────────┐
 │                      game process (Linux)                │
 │                                                          │
 │   original game code  ◄──hooks──►  decompiled C++        │
 │   (mapped from eboot)              (replaces functions)  │
 │          │  imports (by NID)                             │
 │          ▼                                               │
 │   HLE system libraries (ours)                            │
 │   kernel · libc · file I/O · pad · audio · GNM · …       │
 │          │                                               │
 │          ▼                                               │
 │   Linux: pthreads · mmap · files · SDL/evdev · Vulkan    │
 └──────────────────────────────────────────────────────────┘
```

## Components

### Executable extraction
`eboot.bin` is a SELF container. The dump used here stores its segments unencrypted and uncompressed (a "fake-signed" SELF), so a plain ELF can be rebuilt from the container. A tool will extract it into a local analysis directory, never into the repo.

### Loader
- Maps the ELF `PT_LOAD` segments at their expected addresses, or rebases them.
- Parses the PS4-specific dynamic section: imported libraries, modules and symbol NIDs.
- Applies relocations and fills the import tables with our implementations.
- Sets up TLS, the stack and the process parameters, then jumps to the entry point.

### HLE system libraries
Each imported Sony library becomes a module in our runtime. Every function is identified by its NID. Unimplemented functions resolve to a stub that logs the call, which shows us what to implement next. The game's bundled PRX files (`sce_module/`) are either loaded the same way as the eboot or replaced by our code.

The PS4 OS is FreeBSD-based and uses the System V x86-64 ABI, the same as Linux. That makes most calls a direct translation. TLS layout and signal and exception behavior need care.

### Graphics
GNM builds AMD GCN command buffers (PM4 packets). The plan:
1. Parse the command buffers the game submits.
2. Track GPU state and resources (buffers, textures, render targets).
3. Translate GCN shader binaries to SPIR-V with our own translator.
4. Record and submit the equivalent Vulkan work.

### Function replacement
Decompiled functions are compiled into the runtime. At startup, a table of `{original_address, replacement}` pairs is applied. Each original function entry is patched to jump to the replacement. Replacements call other game functions through their original addresses until those are replaced too.

### Differential test harness
Decompiled functions are verified before the game boots. A test process maps the original executable through the loader. It then calls the original function and the C++ replacement with the same inputs, in the same initial memory state, and compares return values and touched memory. Functions with side effects on the OS layer use recorded or stubbed imports.

### Symbols
Our analysis results (names, signatures, types, matched NIDs) live as text in `symbols/`. Scripts sync them with a local Ghidra project, so the project can be rebuilt at any time and the history is tracked in git.

## Non-goals
- CPU emulation or instruction translation.
- Byte-matching decompilation.
- Supporting other games or other game versions.
