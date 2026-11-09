# AGENTS.md

Guidance for coding agents working on this repository. Humans should start with `README.md`.

## Goal

Reverse engineer Bloodborne (PS4, CUSA03173, patch 1.09) and run it **natively on Linux x86-64**, with **no emulation**, as an optimized port. The main goal is the complete game, playable from start to end, with the best performance and quality we can reach. Build on existing work (emulators, community research, open-source code) wherever that is faster, with credit and under compatible licenses.

## Architecture: incremental hybrid

The PS4 CPU is x86-64, so the game's machine code can run directly on a Linux PC. The parts that don't exist on Linux are the PS4 operating system and the GPU.

1. **Loader.** Our own Linux program extracts the ELF from `eboot.bin` (an unencrypted fSELF), maps its segments, applies relocations and resolves imports. It then jumps to the entry point. There is no CPU emulation and no instruction translation.
2. **HLE system libraries.** Every Sony library the game imports (libkernel, libc, file I/O, pad, audio, GNM, …) is reimplemented by us on top of Linux APIs. Imports are matched by NID. Unknown imports resolve to logging stubs.
3. **Graphics.** GNM command buffers are translated to Vulkan. GCN shader binaries are translated to SPIR-V by our own translator.
4. **Targeted function replacement.** Game functions are decompiled into readable C++ where that serves playability, performance or quality: bugs, crashes, bottlenecks, limits such as the frame rate. Each one replaces the original through a hook or trampoline at its address. A complete decompilation is not a goal, so the original machine code keeps running for everything else.

The goal is **functional equivalence**, not byte-matching. Sony's compiler is not available to us.

See `docs/architecture.md` and `docs/roadmap.md`.

## Hard rules

### Sources and licenses
**Read [`docs/source-policy.md`](docs/source-policy.md) (the source and license policy) before reusing external code.** In short:
- **Never** look at leaked Sony SDK material (also when it is copied into another project), and never use game data other than the maintainer's own dump.
- Everything else is allowed: public docs and NID lists, PS4 emulators (for example shadPS4), Mesa and other open-source code, and Bloodborne/FromSoftware reverse-engineering, decomp and modding material.
- The project is GPL-2.0-or-later. Code is copied or adapted only from files whose license keeps it so (see `docs/source-policy.md`). Whole files go unchanged into `third_party/` with their license; adapted code keeps its copyright and license notices and gets an SPDX tag, a `// Source:` line and a `// Changed for bloodborne-linux, <date>` line.
- Log every source in `docs/references.md` in the same change that uses it.
- Import names need a NID that matches the documented hash; record the name's source in `symbols/nids.csv`.
- If a source's license is unclear, don't copy from it. Ask the maintainer.

### Game version
All addresses refer to `eboot.bin` of CUSA03173 patch 1.09. Its SHA-256 is listed in `docs/source-policy.md`. Tools must verify it.

### Game data
- The game is provided by the user from their own dump. Its location comes from the environment variable `BB_GAME_DIR`, or from `local.env` (untracked) in the repo root.
- Treat the game directory as **read-only**. Never write into it.
- **Never commit** game files, extracted assets, the ELF, disassembly or decompiler output dumps, or large hex excerpts. Short excerpts needed to explain something are fine.
- Never hard-code absolute paths in code, scripts or docs.

### Privacy and repository hygiene
- No personal information in commits, issues, PRs or files: no real names, email addresses, usernames in paths, or home-directory paths. Exception: third-party copyright and license notices kept under `docs/source-policy.md` (in `third_party/` and in header comments of adapted code). They never contain the maintainer's own data.
- Commits use the repository-local identity, the GitHub noreply address. Check with `git config user.email` before committing.
- Agent or AI tool files are never committed. `AGENTS.md` is the only exception.
- The git hooks in `tools/githooks/` enforce part of this. Never bypass them (`--no-verify`).
- **Never open issues, PRs, discussions or comments on third-party repositories** without explicit approval from the maintainer.

## Task tracking

- Work is tracked in GitHub issues on this repo, grouped by the milestones in `docs/roadmap.md`.
- Labels: `loader`, `hle:kernel`, `hle:gnm`, `formats`, `decomp`, `tooling`, `research`, `review`.
- At the end of every work session, post a handoff comment on the issue: done, open, next step, gotchas.
- If an issue can only start after another one is merged, add a line `Depends on #N` to its body. An issue starts only once every issue it depends on is merged into `main`. If a dependency goes to the maintainer instead of merging, the issues that depend on it wait as well.
- One issue per task with one checkable result, normally one PR. Per-function progress goes in `symbols/functions.csv`, not in issues.
- Agents may create and comment on issues in this repository. Close issues through commits (`Fixes #N`).
- Issue text follows the same privacy rules as commits.

## Workflow: decompiling one function

1. Pick a function that an open issue needs for playability, performance or quality.
2. Analyze it in Ghidra. Record names, signatures and structs there, then export them to `symbols/`. The text files in `symbols/` are the source of truth, and the Ghidra project is a disposable local cache.
3. Write the C++ version in the matching source file. Name it after what it does and add a comment with the original address: `// 0x00XXXXXX`.
4. Write a differential test. The harness calls the original function (mapped from the executable) and the replacement with the same inputs and compares return values and touched memory.
5. Register the replacement hook.
6. Update `symbols/functions.csv` (status `decompiled` once tests pass, `replaced` once hooked in).

## Decisions and efficiency

Work effectively at optimal efficiency: choose the cheapest way (models, effort, number of agents, review rounds) that still delivers correct, tested work. There is no fixed token budget.

Agents decide on their own how to cut and bundle issues and PRs, which models and effort to use, design questions inside a milestone, and when to start the next milestone or part. These go to the maintainer: anything that fails the merge gate, guardrail changes, license questions `docs/source-policy.md` doesn't answer, changes to the goal or architecture above, and milestone tags.

## Reviews

Risky code is never reviewed only in the session that wrote it.

1. **Automatic:** git hooks on every commit. From M2, CI (build with sanitizers, unit and differential tests, privacy check, and formatting and lint once there is code).
2. **Per decompiled function:** a differential test. Non-trivial functions also get an independent review that compares the C++ against the original disassembly without seeing the author's reasoning. Trivial means a leaf function of a few instructions that only reads or writes fields or returns a constant.
3. **Per PR:** every change goes through a PR. The session that opens it picks the review class and names it in the PR body:
   - **Core** (shader translator, loader, ABI, memory mirror, decompiled functions, interfaces other issues build on): an independent review of the PR diff in a fresh context at high effort, up to two fix rounds.
   - **Normal** (all other code): one independent review in a fresh context, one fix round.
   - **Small** (only docs, `symbols/` or tests, or at most 50 changed lines in total with none in core code): no independent review. A PR that adds a source to `docs/references.md`, copies or adapts external code, or adds NID names from an external source is never Small.

   Findings about correctness, security or licenses block the merge. Style findings are optional. Fix blocking findings in the same PR unless they need their own design work. Keep PRs to at most 800 changed lines of non-test code (docs, `symbols/`, `tests/` and `tools/**/test_*.py` don't count), and put generated data and copied `third_party/` files in a PR of their own (they don't count either). Never remove or weaken a test to meet the limit.
   **Merging:** an agent may merge a PR once it passes every part of the gate:
   - `tools/run-tests.sh` passes, locally with the game where tests need it, and in CI once it exists. `tools/merge-pr.sh` runs it again on the rebased commits, so what lands on `main` is what was tested.
   - `tools/privacy-check.sh` is clean. `tools/merge-pr.sh` runs it too.
   - Every blocking finding of the independent review is fixed, or moved to a linked issue because it needs its own design work, or withdrawn by the reviewer in a re-review. The PR's author can't dismiss a blocking finding on its own. If it disagrees, a second independent reviewer decides from the code and the finding. If that reviewer can't decide, the finding goes to the maintainer.
   - Every function the PR marks `replaced` in `symbols/functions.csv` passed its differential test and, unless trivial, its disassembly review (item 2).
   - The PR stays within the 800-line limit above.
   - The PR doesn't change the guardrails: `AGENTS.md`, `docs/source-policy.md`, `.gitignore`, any `.gitattributes`, `.github/`, `tools/githooks/`, `tools/merge-pr.sh`, `tools/privacy-check.sh`, `tools/run-tests.sh` or `tools/common/toolenv.py` (it finds the game for the tests).
   - Each new source in `docs/references.md` is allowed by `docs/source-policy.md` (no leaked SDK material), as checked by the independent review. For code sources the review also checks that the license column matches the files, that copied or adapted code keeps its notices, that nothing was copied from a file whose license is not on the allow-list in `docs/source-policy.md` (it keeps the program GPL-2.0-or-later), and that `third_party/` holds only unchanged copies plus their license files.

   Anything that fails the gate goes to the maintainer. `tools/merge-pr.sh` refuses guardrail changes and oversized PRs unless the maintainer runs it with `--maintainer`. Agents never pass that flag. Agents always pass `--sha <commit>`, the PR head the independent review saw (for a Small PR, the head that was tested); the script refuses the merge if the branch has moved since.
   **Never use GitHub's merge button or `gh pr merge`**: GitHub writes the account's display name into the commit. Merge with `tools/merge-pr.sh --sha <reviewed head> <N>` instead. It rebases locally, runs the tests and the privacy check (which includes authorship) on the rebased commits, and fast-forwards `main`.
4. **Per milestone:** a dedicated review session over the milestone's range covers:
   - a walkthrough of the new code
   - code health and an architecture check
   - a sample check of `symbols/` quality
   - a source and license audit (`docs/references.md` complete, code sources and `third_party/` match their licenses, no leaked SDK material)
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
