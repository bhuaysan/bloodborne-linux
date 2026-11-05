# AGENTS.md

Guidance for coding agents working on this repository. Humans should start with `README.md`.

## Goal

Reverse engineer Bloodborne (PS4, CUSA03173, patch 1.09) **with our own code** and run it **natively on Linux x86-64**, with **no emulation**.

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
**Read [`docs/clean-room.md`](docs/clean-room.md) before using any external source.** In short:
- **Never** look at leaked Sony SDK material (also when it is copied into another project), Bloodborne or FromSoftware decomp or recomp projects and their symbol, struct or param dumps, or the source of PS4 recomp tools or disassembler loader plugins.
- Public format and hardware docs are allowed. So are some code sources (AMD GPU code, Mesa, FromSoftware file-format material, PS4 emulators), under the license rules in `docs/clean-room.md`: code is reused only from permissive licenses and keeps its notice; copyleft and unlicensed sources give facts only.
- Log every source in `docs/references.md` in the same change that uses it.
- Import names are recovered only by matching hashes of candidate names we can justify ourselves (see the NID section in `docs/clean-room.md`).
- If you're unsure whether a source is allowed, don't open it. Ask the maintainer.

### Game version
All addresses refer to `eboot.bin` of CUSA03173 patch 1.09. Its SHA-256 is listed in `docs/clean-room.md`. Tools must verify it.

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
- Labels: `loader`, `hle:kernel`, `hle:gnm`, `formats`, `decomp`, `tooling`, `research`, `review`.
- At the end of every work session, post a handoff comment on the issue: done, open, next step, gotchas.
- If an issue can only start after another one is merged, add a line `Depends on #N` to its body. An issue starts only once every issue it depends on is merged into `main`. If a dependency goes to the maintainer instead of merging, the issues that depend on it wait as well.
- One issue per task of about a day or more. Per-function progress goes in `symbols/functions.csv`, not in issues.
- Agents may create and comment on issues in this repository. Close issues through commits (`Fixes #N`).
- Issue text follows the same privacy rules as commits.

## Repository layout

```
AGENTS.md            this file
README.md            project overview for humans
CMakeLists.txt       C++ build (CMakePresets.json: debug, relwithdebinfo, release)
docs/
  architecture.md    system design
  roadmap.md         milestones and tracks
  clean-room.md      reference policy, NID naming, game version hash
  references.md      credit log for every external source
  formats/           our own notes on PS4 and game file formats
  design/            design notes (runtime.md: loader, stubs, hooks, harness)
src/                 the runtime, C++20 (see docs/design/runtime.md)
  common/            file reading, SHA-256, game directory lookup
  loader/            SELF to ELF, ELF parsing, mapping, relocation
  hle/               HLE module registry, import stubs, NID name table
  hook/              function-entry patches and the replacement registry
  game/              decompiled game functions
  harness/           differential test harness (tests only)
  tools/             bb-load
tests/               GoogleTest unit and differential tests, one directory per target, plus support/
symbols/             our analysis as text: functions.csv, nids.csv, types/
tools/
  self2elf/          rebuilds the ELFs from eboot.bin and the bundled .prx files
  elf2std/           rewrites a PS4 ELF into a standard ELF for stock tools
  elfimports/        lists modules, libraries, imports and exports of the ELFs
  nidmatch/          recovers NID names by hashing candidate names
  noreturn/          finds imports that never return, for Ghidra's analysis
  nidtable/          generates the runtime's NID name table from symbols/nids.csv
  ghidra-sync/       syncs Ghidra annotations with symbols/
  common/            shared Python helpers (paths, pinned files, PS4 ELF reader)
  githooks/          privacy checks (enabled via core.hooksPath)
  merge-pr.sh        merges a PR locally after the gate (see Reviews)
  privacy-check.sh   privacy checks over a branch's commits
  run-tests.sh       runs every unit test
.github/             issue templates, CI workflow
```

## Workflow: decompiling one function

1. Pick a function, preferably from an open issue or a leaf function that a replaced function calls.
2. Analyze it in Ghidra. Record names, signatures and structs there, then export them to `symbols/`. The text files in `symbols/` are the source of truth, and the Ghidra project is a disposable local cache.
3. Write the C++ version in the matching source file. Name it after what it does and add a comment with the original address: `// 0x00XXXXXX`.
4. Write a differential test. The harness calls the original function (mapped from the executable) and the replacement with the same inputs and compares return values and touched memory.
5. Register the replacement hook.
6. Update `symbols/functions.csv` (status `decompiled` once tests pass, `replaced` once hooked in).

## Reviews

Code is never reviewed only in the session that wrote it.

1. **Automatic:** git hooks on every commit. From M2, CI (build with sanitizers, unit and differential tests, privacy check, and formatting and lint once there is code).
2. **Per decompiled function:** a differential test, plus an independent review that compares the C++ against the original disassembly without seeing the author's reasoning.
3. **Per PR:** every change goes through a PR. Before merging, an independent review runs on the PR diff in a fresh context. Use high effort for loader, ABI and graphics code. Fix review findings in the same PR unless they need their own design work. Keep PRs to at most 500 changed lines of code and tests (docs and `symbols/` don't count), and put generated data in a PR of its own.
   **Merging:** an agent may merge a PR once it passes every part of the gate:
   - `tools/run-tests.sh` passes, locally with the game where tests need it, and in CI once it exists. `tools/merge-pr.sh` runs it again on the rebased commits, so what lands on `main` is what was tested.
   - `tools/privacy-check.sh` is clean. `tools/merge-pr.sh` runs it too.
   - Every finding of the independent review is fixed, or moved to a linked issue because it needs its own design work, or withdrawn by the reviewer in a re-review. The PR's author can't dismiss a finding on its own. A finding it disagrees with goes to the maintainer.
   - Every function the PR marks `replaced` in `symbols/functions.csv` passed its differential test and its disassembly review (item 2).
   - The PR stays within the 500-line limit above.
   - The PR doesn't change the guardrails: `AGENTS.md`, `docs/clean-room.md`, `.gitignore`, any `.gitattributes`, `.github/`, `tools/githooks/`, `tools/merge-pr.sh`, `tools/privacy-check.sh`, `tools/run-tests.sh` or `tools/common/toolenv.py` (it finds the game for the tests).
   - Each new source in `docs/references.md` falls clearly into a category that `docs/clean-room.md` allows, as checked by the independent review.

   Anything that fails the gate goes to the maintainer. `tools/merge-pr.sh` refuses guardrail changes and oversized PRs unless the maintainer runs it with `--maintainer`. Agents never pass that flag. Agents always pass `--sha <commit>`, the PR head the independent review saw; the script refuses the merge if the branch has moved since.
   **Never use GitHub's merge button or `gh pr merge`**: GitHub writes the account's display name into the commit. Merge with `tools/merge-pr.sh --sha <reviewed head> <N>` instead. It rebases locally, runs the tests and the privacy check (which includes authorship) on the rebased commits, and fast-forwards `main`.
4. **Per milestone:** a dedicated review session over the milestone's range covers:
   - a walkthrough of the new code
   - code health and an architecture check
   - a sample check of `symbols/` quality
   - a clean-room audit (`docs/references.md` complete, no names of unclear origin)
   - a privacy audit of the history
   
   Small fixes go into a follow-up PR. Bigger findings become issues labeled `review`. Then the maintainer tags the milestone.

## Builds, branches and data

There are no separate dev and prod environments. The differences live in build presets and data locations.

### Build presets (from M2, `CMakePresets.json`)
- `debug`: the everyday build. AddressSanitizer + UBSan, verbose logging of imported calls, extra checks in the test harness.
- `relwithdebinfo`: optimized and debuggable. For playtesting and profiling.
- `release`: optimized. What other people build.

### Branches
- `main` always builds and passes its tests.
- Work happens on short-lived feature branches (`feat/…`, `fix/…`, `docs/…`) merged through PRs, so CI runs.
- The maintainer tags each completed milestone (`m1`, `m2`, …) after its review. Agents never create tags.
- No long-lived `develop` branch.

### CI (from M2)
GitHub Actions builds every preset, runs the unit tests and the privacy checks. CI never has game files, so tests that need the game run locally only and must skip cleanly when `BB_GAME_DIR` is unset.

### Data locations
- Game dump: read-only, from `BB_GAME_DIR`.
- Runtime output (save data, shader cache, logs, config) goes to XDG directories, e.g. `$XDG_DATA_HOME/bloodborne-linux/` and `$XDG_CACHE_HOME/bloodborne-linux/`. Never into the game or repo directory.
- `--profile <name>` keeps separate save data per profile, so test saves stay apart from a real playthrough.
- `analysis/` and the Ghidra project are disposable. `symbols/` is the source of truth.

## Conventions

- Language: C++20 for the runtime and decomp, Python 3 for tools. Build with CMake + Ninja.
- Documentation and code comments are in English.
- Commit messages are in the imperative mood, under 72 characters, and say what changed. Reference issues with `Fixes #N` / `Refs #N`.
- Keep changes small and focused, one logical change per commit.
