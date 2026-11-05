# Core OS design (M3)

How the runtime carries the game from its entry point to its main loop: loading the bundled PRXs, TLS, threads, sync, memory, time, files, runtime directories and logging. It builds on [runtime.md](runtime.md) (loader, stubs, hooks, harness). Issues of milestone M3 build on this note. Change it in the same PR when an implementation has to deviate.

Facts marked *(survey)* come from our static analysis of the pinned build; addresses are link addresses of the eboot unless a PRX is named.

## Goal: "main loop reached"

`main` (0x1e4a6c0) runs its init and then loops `while (tick()) {}` at 0x1e4a6f0. The tick (0x2018d20) has that loop as its only direct caller. Each tick calls the frame pacer (0x2034770, 0x820 bytes, only caller the tick), which reads the wall clock through `gettimeofday` (libScePosix) and sleeps through `sceKernelUsleep` until 1/30 s has passed *(survey)*.

**Criterion.** The main loop is reached when `gettimeofday` has been called 60 times from a return address inside 0x2034770–0x2034f90, while `main` has not returned. The HLE `gettimeofday` checks its return address against that range, so the check needs no hooks and no graphics. `bb-run --stop-at-main-loop` logs `main loop reached after <n> s` and exits with status 0 at that point, through the `_exit` path (flush the log, then `_exit(0)`), never `std::exit`: worker threads are still running, and host `atexit` handlers, static destructors and LeakSanitizer must not run under them. The game test runs exactly that with a timeout.

What must work before that point: PRX init, TLS, threads, mutexes, rwlocks and condition variables (the task system starts worker threads), kernel memory mapping (the eboot maps a 116 MiB block itself), an advancing wall clock and sleeps, and success results from `sceSysmoduleLoadModule(0xb4)`, `sceAppContentInitialize` and `sceUserServiceInitialize`. Network and PSN initialisation (`sceNetInit`, `sceHttpInit`, …) sit on the init path too *(survey)*.

## Loading the bundled PRXs

- **Load set.** eboot, `libSceFios2.prx` and `libc.prx`, each checked against the hashes in `docs/clean-room.md`. The PRXs are mapped one after the other above the eboot, each at the next address aligned to its largest `p_align`. The other five bundled PRXs are not loaded.
- **Binding.** An import resolves, in this order, to: an export of a loaded module with the same library name and NID, an HLE implementation, a stub. `libSceLibcInternal` imports of libSceFios2 resolve to the libc.prx export with the same NID when one exists (15 of 16 do; a NID is the hash of the name alone). One HLE layer less, and the two libraries share one heap.
- **Exports go through `bb::hle`.** The load set maps all three modules first. It then registers every export of the PRXs with the HLE layer (`bb::hle::register_export(library, nid, address)`; libc.prx's exports a second time under `libSceLibcInternal`, unless that library exports the NID itself; `STT_TLS` exports are skipped, since base + `st_value` is no address for them), and only then relocates the three images with `bb::hle::resolve`. `resolve` applies the order above. So runtime.md's numbered steps stay as they are (only its game-runtime row grows), and `import_address` still returns exactly what the loader wrote into the GOT. Registering an export for an import that was already resolved, or registering it twice, is a `std::logic_error`, per (library, NID) as for implementations. An HLE implementation that an export shadows is logged once at start.
- **Module ids.** Loading order defines the TLS module id: eboot 1, libSceFios2 2, libc 3. All `R_X86_64_DTPMOD64` relocations of the PRXs (14 in libc, 3 in libSceFios2) name symbol 1, a local section symbol with an empty name and `st_shndx` 0 *(survey)*. `relocate` accepts `R_X86_64_DTPMOD64`, ignores its symbol and writes the id of the module being relocated, which it takes as a parameter. The offset cell next to each one is a link-time constant: there are no `R_X86_64_DTPOFF64` relocations.
- **Init.** In both PRXs `DT_INIT` is a real function at link address 0 (also `e_entry`) with the signature `(rdi, rsi, fn)`. It runs the preinit array and the constructor list, and then tail-calls `fn(rdi, rsi)` if `fn` is non-null, otherwise `module_start` *(survey)*. The runtime calls it as `init(0, nullptr, nullptr)`, on the game's main thread before the entry call (see Entry), libSceFios2 first, then libc. The two modules import from each other, so the order is not a dependency order. It is harmless because libSceFios2's init does nothing: its preinit array and constructor list are empty and its `module_start` cell is 0 *(survey)*. A non-zero return aborts the start.
- **libc runs the game's allocator.** libc's preinit entry asks libkernel for the process parameters (NID `959qrazPIrg`) and gets the runtime address of the eboot's `PT_SCE_PROCPARAM` block. Its +0x38 points to a 0x78-byte block (version 9 at +8, 1 at +0xc). The block's +0x30 points to a table of 12 functions in the eboot (0x20809d0–0x2081490), and its +0x60 to a second table of 5 (0x20814a0, 0x20814b0, 0x20814c0, 0x20817a0, 0x2081640). libc copies them into its dispatch cells, so its `malloc` family calls the game's own allocator *(survey)*. During preinit libc calls only the first entry of each table, both no-ops. The game's heap, a 116 MiB direct-memory block (allocate and map), is set up lazily on the allocator's first allocation *(survey)*, on whichever thread allocates first. Whether a libc constructor allocates during init is not known. PRX init runs on the game's main thread anyway, so all game code, the allocator included, runs on threads that have the eboot's TLS block and go through the same start-up path.
- **Eboot init.** The eboot's entry calls its own `DT_INIT`, so the runtime never does.
- **Fini.** PRX `DT_FINI` is not run at process exit in M3. libc's exit handler already flushes stdio, and the process ends anyway.

## Entry

The game's main thread is a pthread created by the runtime with a 16 MiB stack, not the process's initial thread, so the stack size is ours to choose and the initial thread can watch the timeout. It first runs the PRX initialisers (see Init), then calls the entry 0xa0 as `entry(args, exit_handler)`: `rdi` points to `{int argc; char* argv[]}` with `argv[0] = "/app0/eboot.bin"`, and `rsi` is a function the entry registers with `atexit` *(survey)*. libc's `exit` calls it, so it must be valid: the runtime passes a function that does nothing (PRX `DT_FINI` is not run in M3). `exit` ends in our `_exit`, which flushes the log and calls `_exit(2)`.

## TLS: one thread pointer for game and runtime

The eboot uses initial-exec TLS only: `mov rax, fs:0` followed by adding a constant from one of 15 GOT cells, all in [-0x750, -0xb]. That is its 0x750-byte, 16-aligned `PT_TLS` block directly below the thread pointer, variant II [Drepper]. It never reads `fs:0x28`. Its stack guard is the libkernel data import `f7uOxY9mM1U`. The PRXs contain no `fs` accesses. They reach their TLS through the libkernel helper `vNe1w4diLCs` with a `{module, offset}` pair (general dynamic), which the eboot imports but never calls *(survey)*.

**Decision.** Game code runs with glibc's thread pointer. `fs:0` is the self pointer in glibc too.
- The eboot's block is reserved as one 0x750-byte, 16-aligned `thread_local` that the linker puts at the top of the executable's TLS segment. It lives in an `OBJECT` library linked into `bb-run` and into every test executable that runs game code (the game tests, and the harness executables once they run PRX init or fs-using eboot functions; for those, this lifts runtime.md's M2 "no TLS" limit). At start, and in every new thread, each of them checks that the block ends exactly at the thread pointer. Otherwise it aborts with an error that names both addresses. glibc zero-fills it per thread, which matches the eboot's template (file size 0).
- The TLS helper hands out per-thread, per-module blocks initialised from the module's `PT_TLS` template. It creates them on first use and frees them at thread exit. Its `{module, offset}` argument is not always 8-byte aligned (libSceFios2 passes 0x4c084, for example *(survey)*), so the helper reads it with `memcpy`, never through a struct pointer.
- **Why:** there is no `fs` switching anywhere, so HLE calls, hooks, callbacks and signal handlers need no glue (this closes the TLS point that runtime.md left open). The other option, a PS4-layout thread block per thread with `wrfsbase` on every transition, needs glue at every boundary and breaks signal handlers that run on the wrong `fs`.

## Threads

- The thread handle of the scePthread functions and the POSIX `pthread_t` of libScePosix are pointers to our `Thread` object (host `pthread_t`, name, attributes, return value). The sce and POSIX entry points share one implementation and differ only in how they report errors.
- Stack size and detach state are applied. Priority, scheduling policy and affinity are recorded and logged, not applied. Names go to `pthread_setname_np` (cut to 15 bytes) and into the log.

## Sync

- Mutexes, rwlocks and condition variables: the game's object holds a pointer to our object, which wraps the glibc primitive of the matching type. An object still holding its static-initialiser value is created on first use, under a global lock. The issues confirm the guest object size and initialiser values from the game and libc.prx code.
- The counted sync object (create/destroy/wait/post) and the event queue (create/destroy/wait, triggered from our own code, e.g. a flip) are our own objects on a mutex and a condition variable.
- Error results: see [Errors](#errors).

## Errors

Every POSIX entry point (threads, sync, files, memory, time) reports FreeBSD `errno` values, in the convention of its family: calls in the style of `open`, `read`, `mmap` and `clock_gettime` return -1 and set the guest `errno`; `pthread_*` calls return the error number and leave `errno` alone. Every sce entry point returns the sce error encoding and leaves `errno` alone, unless the ABI issue finds a caller in the game or libc.prx that reads `errno` after one; it then writes that down. Host `errno` values are translated through one table in `src/hle/kernel/`. A POSIX call that fails sets the guest `errno`: the per-thread `int` whose address the libkernel errno-location import hands to libc.prx and the eboot, never glibc's `errno`. The `errno` values, the flag values, clock ids and struct layouts, each with its source, are in `docs/formats/kernel-abi.md`, taken from FreeBSD's errno(2), FreeBSD's public system headers (allowed by `docs/clean-room.md` since #77, under the limits written there: each value confirmed against the game's or libc.prx's use, release and header files logged in `docs/references.md`) and how libc.prx converts results.

## Memory

- Kernel map calls (`mmap`, the anonymous and named variants the allocator uses) become host `mmap` calls with translated flags. The host picks addresses: every observed call site passes flags 0, no fixed address *(survey)*.
- **Direct memory** (allocate a range of "physical" memory, map it, release it, query the total size): backed by one `memfd` of fixed size, with the allocations as offset ranges in it. Mapping maps the memfd at that offset. Mapping the same range twice aliases, as it must. The protection bits above `PROT_EXEC` (0x30, used together with read/write) are accepted and dropped.
- The memfd is created sparse with the size that the total-size query returns. The direct memory issue fixes that size, the allocation alignment and how the search range of an allocation works, from the game's call sites, and writes them down here.
- **Harness.** The harness doesn't load the PRXs by default, so eboot imports of libc go to stubs as in M2. Testing a function that allocates needs libc's `malloc` family, which needs libc.prx loaded and initialised, and with it the game's allocator. The issue that adds this loads the load set and runs the PRX init once in the fixture setup, then forces the game's heap into existence with one allocation and its release, and creates the test thread's TLS blocks for both PRX modules through the TLS helper, all before the baseline snapshot. Otherwise the first test would map the heap or create a TLS block after the baseline, the restore would reset the allocator's state, and the next call would map it again. The 116 MiB heap doesn't fit runtime.md's 16 MiB arena. It becomes a snapshot region of its own. So do the other state the PRXs and TLS add, which `Harness(Image&)` doesn't cover today: the writable segments of libc.prx and libSceFios2 (libc's dispatch cells, its stdio state, and the libc data exports that eboot data imports now bind to instead of data blocks), the test thread's eboot TLS block, the PRX TLS blocks and the guest `errno`. The issue decides how to keep the per-test cost of these regions low and updates runtime.md's harness section.

## Time

Wall clock from `CLOCK_REALTIME`, every counter from `CLOCK_MONOTONIC`, sleeps through `clock_nanosleep`. Clock ids and the counter frequency are confirmed from the game's use. libSceRtc is implemented from the game's call sites only (see Naming).

## Files and paths

Guest paths are mapped through a mount table. Everything else is refused with `ENOENT` and logged once per prefix.

| Guest | Host | Access |
|---|---|---|
| `/app0/…` | `$BB_GAME_DIR/…` | read-only; writes fail with `EROFS` |
| save data mount points handed out by our libSceSaveData | `$XDG_DATA_HOME/bloodborne-linux/profiles/<profile>/savedata/<n>/` | read-write |
| temporary mount | `$XDG_CACHE_HOME/bloodborne-linux/profiles/<profile>/temp/` | read-write |
| `/hostapp/…` (debug output) | refused | logged |

Paths are normalised, and `..` cannot leave a mount. When an exact lookup under `/app0` fails, a case-insensitive lookup runs and its hits are cached and logged. macOS junk files (`._*`, `.DS_Store`) are hidden from directory listings. File descriptors are guest numbers in our own table that maps them to host descriptors, so the game can never close or reuse the runtime's own descriptors. Guest 0, 1 and 2 start out mapped to duplicates (`dup`) of the host's stdin, stdout and stderr (libc.prx's stdio writes to them), so a guest `close(2)` closes only the duplicate and the log keeps its stderr. A new descriptor gets the lowest free guest number, as in POSIX.

## Runtime directories

`$XDG_DATA_HOME` (profiles, save data), `$XDG_CACHE_HOME` (temp, later the shader cache), `$XDG_CONFIG_HOME` (config, later) and `$XDG_STATE_HOME` (logs), each with the XDG default when unset, each under `bloodborne-linux/`. `--profile <name>` (`[A-Za-z0-9_-]{1,32}`, default `default`) selects the profile directories. A variable that is empty or relative counts as unset, as the XDG spec says; the defaults need `HOME`. Directories are created with mode 0700 when first used, missing parents included (existing ones keep their mode). Nothing is ever written to the game or repository directory: `bb::common::RuntimeDirs` (`src/common/runtime_dirs.hpp`) refuses to start when one of its four directories and the game directory or the source checkout the binary was built from lie inside one another, and it creates directories only inside its own four.

## Logging

One logger in `bbcommon`: levels, channels (`loader`, `hle.<library>`, `game`), the thread name on every line, output to stderr and to `$XDG_STATE_HOME/bloodborne-linux/logs/<UTC time>-<pid>.log`. `BB_LOG=<channel>=<level>,…` filters. In the `debug` preset every HLE call is logged at trace level. Stub logging follows runtime.md.

## Naming

NIDs without a matched name are implemented under a descriptive C++ name and registered by NID, with a comment that the name is unknown. They get no row in `symbols/nids.csv`, which holds matched names only. For libSceRtc, libSceSaveData and libSceSaveDataDialog (exposure log in `docs/clean-room.md`, which treats libSceSaveDataDialog like libSceSaveData) semantics come from the game's call sites only, and no Sony-style names are written down, not even in comments.

## Open questions

1. Whether the eboot TLS reservation ends exactly at the thread pointer with the sanitizer runtimes linked in (clang links ASan statically). glibc puts the executable's block at the thread pointer minus `p_memsz` rounded up to `p_align`. So the reservation must be the last thing in the executable's TLS segment, and that segment's `p_memsz` must be a multiple of its `p_align`. A `thread_local` with an alignment above 16 anywhere in the executable can break either. If one does, a linker script fragment will be needed.
2. Whether worker threads or the tick block on GPU or flip completion. If they do, M3 needs headless video out and GPU submission (flips complete at once) before the criterion can be met.
3. Whether the game throws C++ exceptions. If so, libc.prx's unwinder needs module information from libkernel (possibly the unnamed info query `f7KBOafysXo`).

[Drepper]: Ulrich Drepper, "ELF Handling For Thread-Local Storage", see `docs/references.md`.
