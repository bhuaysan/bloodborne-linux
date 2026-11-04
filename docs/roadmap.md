# Roadmap

Each milestone matches a GitHub milestone. Tasks are tracked as issues.

| Milestone | Goal | Done when |
|---|---|---|
| **M0 Setup** | Repo, docs, tooling, guardrails | Hooks work, analysis tools installed, first push done |
| **M1 Executable analysis** | Understand the executable | SELF→ELF extractor works; imports, NIDs, modules and segments are listed and documented in `docs/formats/` |
| **M2 Loader** | Map and start the game | The ELF is mapped, relocated, imports resolve to logging stubs, and execution reaches the entry point |
| **M3 Core OS** | Reach the main loop | Threads, memory, sync primitives, time and file I/O work well enough for the game to reach its main loop |
| **M4 Graphics** | Show something on screen | PM4 parsing, GPU state tracking, GCN→SPIR-V translation; the first frame renders |
| **M5 Input & audio** | Playable basics | Controller input and audio output work |
| **M6+ Decompilation** | Replace game code | Functions are decompiled and replaced continuously; progress tracked in `docs/progress/functions.csv` |

Milestones overlap. Decompiling individual functions can start any time it helps understand the game.
