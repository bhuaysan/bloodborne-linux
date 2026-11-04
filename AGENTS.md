# AGENTS.md

Guidance for coding agents working on this repository. Humans should start with `README.md`.

## Goal

Reverse engineer Bloodborne (PS4, CUSA03173, patch 1.09) **from scratch** and run it **natively on Linux x86-64**, with **no emulation**.

## Architecture: incremental hybrid

The PS4 CPU is x86-64, so the game's machine code can run directly on a Linux PC. The parts that don't exist on Linux are the PS4 operating system and the GPU.

1. **Loader.** Our own Linux program extracts the ELF from `eboot.bin` (an unencrypted fSELF), maps its segments, applies relocations and resolves imports. It then jumps to the entry point. There is no CPU emulation and no instruction translation.
2. **HLE system libraries.** Every Sony library the game imports (libkernel, libc, file I/O, pad, audio, GNM, …) is reimplemented by us on top of Linux APIs. Imports are matched by NID. Unknown imports resolve to logging stubs.
3. **Graphics.** GNM command buffers are translated to Vulkan. GCN shader binaries are translated to SPIR-V by our own translator.
4. **Function replacement.** Game functions are decompiled into readable C++ one at a time. Each one replaces the original through a hook or trampoline at its address. Over time the original binary becomes unnecessary.

The goal is **functional equivalence**, not byte-matching. Sony's compiler is not available to us.

See `docs/architecture.md` and `docs/roadmap.md`.

## Hard rules

### Clean room / references
- **Never** look at other Bloodborne or FromSoftware decomp, recomp or modding-reverse-engineering projects.
- **Never** read the source code of PS4 emulators or PS4 recompilation tools.
- **Never** use leaked Sony SDKs, headers, documentation or symbol dumps.
- Allowed with credit: public format and hardware documentation (community wikis about PS4 file formats, AMD GCN ISA manuals, Vulkan / ELF / System V ABI specs, Linux man pages) and general programming resources.
- Log every external source you consult in `docs/references.md`: source, what it was used for, and the date. Do this in the same change that uses it.
- When unsure whether a source is allowed, don't open it. Ask the maintainer.

### Game data
- The game is provided by the user from their own dump. Its location comes from the environment variable `BB_GAME_DIR`, or from `local.env` (untracked) in the repo root.
- Treat the game directory as **read-only**. Never write into it.
- **Never commit** game files, extracted assets, the ELF, disassembly or decompiler output dumps, or large hex excerpts. Short excerpts needed to explain something are fine.
- Never hard-code absolute paths in code, scripts or docs.

### Privacy and repository hygiene
- No personal information in commits, issues, PRs or files: no real names, email addresses, usernames in paths, or home-directory paths.
- Commits use the repository-local identity, the GitHub noreply address. Check with `git config user.email` before committing.
- Agent or AI tool files are never committed. `AGENTS.md` is the only exception.
- The git hooks in `tools/githooks/` enforce part of this. Never bypass them (`--no-verify`).
- **Never open issues, PRs, discussions or comments on third-party repositories** without explicit approval from the maintainer.

## Task tracking

- Work is tracked in GitHub issues on this repo, grouped by milestones M0–M6 (see `docs/roadmap.md`).
- Labels: `loader`, `hle:kernel`, `hle:gnm`, `formats`, `decomp`, `tooling`, `research`.
- One issue per task of about a day or more. Per-function progress goes in `docs/progress/functions.csv`, not in issues.
- Agents may create and comment on issues in this repository. Close issues through commits (`Fixes #N`).
- Issue text follows the same privacy rules as commits.

## Repository layout

```
AGENTS.md            this file
README.md            project overview for humans
docs/
  architecture.md    system design
  roadmap.md         milestones
  references.md      credit log for every external source
  formats/           our own notes on PS4 and game file formats
  progress/          function tracker (functions.csv)
tools/
  githooks/          privacy checks (enabled via core.hooksPath)
.github/             issue templates
```

Source directories (`src/`, `tools/<name>/`) will be added as work begins.

## Workflow: decompiling one function

1. Pick a function, preferably from an open issue or a leaf function that a replaced function calls.
2. Analyze it in the disassembler. Record what you learn (name, signature, structs) in the analysis database, not in the repo.
3. Write the C++ version in the matching source file. Name it after what it does and add a comment with the original address: `// 0x00XXXXXX`.
4. Register the replacement hook.
5. Verify it. The game must behave the same, and where possible compare against the original with a test harness or trace.
6. Update `docs/progress/functions.csv`.

## Conventions

- Language: C++20 for the runtime and decomp, Python 3 for tools. Build with CMake + Ninja (added when coding starts).
- Documentation and code comments are in English.
- Commit messages are in the imperative mood, under 72 characters, and say what changed. Reference issues with `Fixes #N` / `Refs #N`.
- Keep changes small and focused, one logical change per commit.
