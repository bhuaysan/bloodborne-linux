# Unnamed core OS imports

What the unnamed firmware imports of the static load set (eboot, libc.prx, libSceFios2.prx) do, as far as their callers show. M3 implements them. Names come only from hash matching (see the NID section of [clean-room.md](../clean-room.md)), so this note gives NIDs and behaviour, never names. The implementing issue registers each NID under a descriptive C++ name.

Addresses are link addresses of the file that makes the call. "Survey" marks facts from the M3 survey that the implementing issue still checks at the call sites; the other rows were read at the call sites given.

## Named in M3

Three of the NIDs in scope got a name by hand-picked candidates from allowed documentation, each with its reason in `symbols/candidates/`:

| Library | NID | Name | Source label | Use |
|---|---|---|---|---|
| libkernel | `vNe1w4diLCs` | `__tls_get_addr` | `drepper-tls` | General-dynamic TLS helper of libc.prx and libSceFios2.prx: `rdi` points to a `{module, offset}` pair whose first cell has an `R_X86_64_DTPMOD64` relocation (libc.prx 0x692, 41 sites; libSceFios2.prx 0x1b50, 16 sites) |
| libkernel | `9BcDykPmo1I` | `__error` | `freebsd-intro2` | No arguments; returns the calling thread's `int*` errno (libc.prx 0x73f6 stores 9 there, libSceFios2.prx 0x2cf86 reads it, eboot 0x64c17) |
| libkernel | `f7uOxY9mM1U` | `__stack_chk_guard` | `gcc-internals` | Data: the stack-protector canary every pinned ELF imports. libc.prx loads it in prologues and compares it in epilogues before `__stack_chk_fail` (function 0x1040: load at 0x1053, compare at 0x107e) |

`src/hle/kernel/tls.cpp` implements `__tls_get_addr` (as `tls_block_address`), `src/hle/kernel/process.cpp` the other two (design note, TLS and Process).

The other NIDs stay unnamed. The FreeBSD manual page lists and the derivation rules found no further names for them.

## libkernel

### Process and libc support

| NID | Callers | Behaviour |
|---|---|---|
| `959qrazPIrg` | libc.prx preinit | Returns the runtime address of the eboot's `PT_SCE_PROCPARAM` block (see the design note, "libc runs the game's allocator") *(survey)* |
| `p5EcQeEeJAE` | libc.prx 0x1fb58, once at init | `rdi` = a static 3-slot table (libc.prx 0xba1e0), filled from the allocator tables the eboot supplies in its process parameters (entries +0x20, +0x28, +0x50, or +0x20, +0x30, +0x28 of the fallback table). Result ignored. Hands the kernel side callbacks from those tables |
| `OMDRKKAZ8I4` | libc.prx 0x5c4, 0x1f56b, 0x1f823, 0x1fb9e, 0x1fbc6, 0x23d58, 0x60027, 0x61357, 0x61367 | Error report: `(code, 0)` with a code 0xa00200xx. The wrapper at 0x1f560 has no code after its call (it falls into padding), so at least there it ends the process |
| `zE-wXIZjLoM` | libc.prx 0x64b, 0x657, 0x1f70b, 0x1f7c4, 0x5ff2a, 0x5ff8a, 0x603cb, 0x611ab | Status report: `(code, 0)` with a code 0xa00200xx; returns. libc's exit paths call it right before `_exit` (0x5ff2a: 0xa0020003 for status 0, 0xa0020004 otherwise) |
| `Wl2o5hOVZdw` | libc.prx 0x1fe83, 0x22494, 0x268af; tail call at 0x1fe10 | `rdi` = a tag string (the name printed as `[%s]` in the report before it, or `"SceLibc"`). Result ignored. Called once after a multi-line diagnostic report printed through libc's printf: ends the report for that tag |
| `DRuBt2pvICk` | libc.prx 0x2729a, in libc's `read` wrapper (0x27210) | `(0, buffer, count)`: the fast path for fd 0 (console input). The result is returned unchanged, without errno conversion. Other descriptors take the general path |
| `FxVZqBAA7ks` | libc.prx 0x27362, in libc's `write` wrapper (0x27340) | `(fd, buffer, count)` for fd 1 and 2: direct console write. The result is returned unchanged |
| `Fjc4-n1+y2g` | libc.prx 0x2f39e, in the exit-handler runner (0x2f2e0–0x2f52e) | `rdi` = a module-info record filled by another import (0x450 stub, called with flag 2 at 0x2f327), `rsi` = a handler address. Non-zero means the handler lies in that module, so it runs when the module unloads |
| `kbw4UHHSYy0` | libc.prx 0x2f506 | `rdi` = the same module-info record. Releases it. Called only when the fill succeeded and the import is bound (libc tests its GOT cell 0xb85b8 for non-zero at 0x2f4f7, a weak import) |
| `crb5j7mkk1c` | libc.prx 0x54530 (function 0x54510) | `rdi` = a 64-bit value from an object's +0x18. Result 2 and 1 select object states 3 and 2, anything else 0. Classifies a handle |
| `OjWstbIRPUo` | libc.prx 0x55810 (tail jump from a libc export) | `rdi` = an object's resource pointer (+0x148). Result ignored. Releases it on the object's failure path |
| `f7KBOafysXo` | libc.prx | `(path, 1 or 2, 0x118-byte result)`, possibly a file or module query. The unwinder may need module information from it (design note, open question 3) *(survey)* |
| `6xVpy0Fdq+I` | libc.prx 0x2fcc6 (`setjmp`), 0x2fd17 (`longjmp`) | Takes the arguments of `sigprocmask`: `(1, NULL, jmp_buf + 0x48)` saves the mask, `(3, jmp_buf + 0x48, NULL)` restores it. The 16 bytes at +0x48 hold the signal set |

### Threads

| NID | Callers | Behaviour |
|---|---|---|
| `W0Hpm2X0uPE` | eboot, libSceFios2.prx | Sets a priority-like thread attribute *(survey)* |
| `1tKyG7RlMJo` | eboot, libSceFios2.prx | Thread attribute call, as far as its call sites show *(survey)* |
| `12wOHk8ywb0` | eboot | Thread attribute call, as far as its call sites show *(survey)* |

### Counted sync object and event queue

| NID | Callers | Behaviour |
|---|---|---|
| `188x57JYp0g` | eboot | Create a counted sync object: name, attribute 1 or 2, initial count, maximum count *(survey)* |
| `R1Jvn8bSCW8` | eboot | Destroy it *(survey)* |
| `Zxa0VhQVTsk` | eboot | Wait: count, timeout pointer *(survey)* |
| `4czppHBiriw` | eboot | Post: count *(survey)* |
| `D0OdFMjp46I` | eboot | Create an event queue with a name ([kernel-abi.md](kernel-abi.md#event-queue)) |
| `jpFjmgAC5AE` | eboot | Delete an event queue ([kernel-abi.md](kernel-abi.md#event-queue)) |
| `fzyMKs9kim0` | eboot | Wait on an event queue: queue, event array, capacity, `&count`, timeout pointer ([kernel-abi.md](kernel-abi.md#event-queue)) |

### Time

| NID | Callers | Behaviour |
|---|---|---|
| `4J2sUJmuHZQ` | eboot 0x33c954, 0x33c969, 0x3a1875, 0x3a18be; libc.prx 0x2ef60 | No arguments, returns a u64 time in microseconds. libc's export `clock` (0x2ef60) is a tail jump to it, and libc's own `getrusage`-based counterpart (0x2eef0) returns microseconds (seconds × 1000000). The eboot uses differences as spin-wait timeouts: 100000 at 0x33c977, 10000 at 0x3a18b5. Implemented as microseconds of `CLOCK_MONOTONIC` since the runtime started |
| `-2IRUCO--PM` | eboot 0x21b9850, 0x21b99bf | No arguments, returns a u64. Both callers add a u32 field (+0xa0) and take the sum modulo a ring size as the start index of a slot search, so the unit doesn't matter to them. Implemented as nanoseconds of `CLOCK_MONOTONIC` since the runtime started, from the same origin |
| `QBi7HCK03hw` | eboot 0xd1a785, 0xd1a815; libSceFios2.prx 0x2cc50 | `(clock id, timespec*)`, always clock 4 (`CLOCK_MONOTONIC`). Returns 0 on success: the eboot returns its own error 0x21 for anything else and converts `tv_sec`/`tv_nsec` to µs and ms, Fios converts them to ns. Implemented as `clock_gettime` with an sce result: 0, or the sce code of `EINVAL` for an unknown clock id and of `EFAULT` for a null pointer |
| `kOcnerypnQA` | libc.prx 0x1939, 0x91f4, 0x9a6f | Fills two int32 values in an 8-byte buffer (zeroed by the caller); result ignored. Callers add both and multiply by 60 to get the offset from UTC to local time in seconds, and set `tm_isdst` when the second is non-zero: minutes east of UTC in standard time, and the daylight saving shift in minutes. Implemented from the host's local time zone, with a shift of 60 while daylight saving time is in effect, else 0 |

### Virtual and direct memory

`src/hle/kernel/vmem.cpp` implements `sceKernelMunmap` and `sceKernelMprotect` so far. The calls in this table still go to logging stubs; where a row says what the runtime does with a call, that is the plan (design note, Memory).

| NID | Callers | Behaviour |
|---|---|---|
| `mL8NDH86iQI` | libc.prx 0x23ec0, 0x23ff9, 0x24932 | Named anonymous map for libc's arena growth: `(&addr, length, prot, flags, name)`. `addr` is 0 on entry and the mapping on return; prot 3, flags 0. 0 means success; on failure libc releases `(addr, length)` with `sceKernelMunmap` anyway. At 0x23ed9 it then calls `sceKernelMlock` on the mapping if a libc flag is set, and releases the mapping when that fails |
| `IWIBBdTHit4` | eboot 0x2081dbe | Anonymous map: `(&addr, length, prot, flags)`, `addr` 0 on entry, length rounded up to 0x4000, prot 3 or 0x33, flags 0. Failure is a non-zero result or `addr` still 0. Released with `teiItL2boFw` |
| `rVjRvHJ0X6c` | eboot 0x20825f3, 0x26bae5b, 0x26d3f5e, 0x26d4066, 0x26d423e, 0x2902024, 0x290316b, 0x290328c | `(address, 0, &record, 0x48)`: the mapping that holds the address. 0 means success. Record fields read: +0 start (u64), +0x10 the direct-memory offset of start (u64; `address + offset - start` at 0x2902063 and 0x2903295, released as direct memory at 0x26d3f67), +0x20 a flag byte: bit 1 direct memory (released through `sceKernelMunmap` and the direct-memory release at 0x20826fd), bit 0 memory from `IWIBBdTHit4` (released through `teiItL2boFw` at 0x2082721). Nothing else is read. The runtime is to fill +8 with the exclusive end |
| `BC+OG5m9+bw` | eboot 0x26bae74 | `(offset, &int32, &u64, &u64)` with the record's +0x10 offset after `rVjRvHJ0X6c`. 0 means success; the caller then compares the int32 with 3. A query on direct memory. The runtime is to fail it until #97 tracks direct memory |
| `teiItL2boFw` | eboot 0x2082721 | `(address, length)`: releases memory from `IWIBBdTHit4` (record flag bit 0). Result ignored |
| `WFcfL2lzido` | eboot 0x26bad98, 0x26badf8, 0x26bb5ee, 0x26bb60f, 0x29031df | `(address, &start or NULL, &end or NULL, &flags)`. 0 means success. Reports the bounds of the mapping that holds the address and its protection bits as a u32 mask; callers test bit 0 (0x26bada4) and bit 4 (0x26bae04) after allocating a block. |
| `pO96TwzOm5E` | eboot | Total size of direct memory *(survey)* |
| `rTXw65xmLIA` | eboot | Allocate direct memory: search start, end, length, alignment, type, `&offset` *(survey)* |
| `L-Q3LEjIbgA` | eboot | Map direct memory: `&addr`, length, prot, flags, offset, alignment *(survey)* |
| `MBuItvba6z8` | eboot | Release direct memory *(survey)* |
| `2SKEx6bSq-4`, `7oxv3PPCumo` | eboot | Possibly direct-memory calls; #97 checks their call sites *(survey)* |

## libScePosix

| NID | Callers | Behaviour |
|---|---|---|
| `Jmi+9w9u0E4` | eboot | Thread creation with a name, next to `pthread_create` *(survey)* |

## libSceLibcInternal (data)

| NID | Callers | Behaviour |
|---|---|---|
| `2sWzhYqFH4E` | libSceFios2.prx GOT cell 0x4c0fc, used at 0x2cad5 | libc's standard-output stream object. libc.prx exports the same NID (0xb9148, initial flags word 0x100010402, a lock registered as `"FileStdout"`), and the eboot imports it from libc. So the libSceLibcInternal fallback of the design note binds it to libc.prx's object |
| `ZT4ODD2Ts9o` | one `R_X86_64_64` cell in each bundled PRX, next to the module's own handle cell (libSceFios2.prx 0x4caf0) | A data object whose address each module stores without reading it itself. Type unknown |

## libSceDiscMap (libSceFios2.prx only)

| NID | Callers | Behaviour |
|---|---|---|
| `ioKMruft1ek` | libSceFios2.prx 0x29c6f, 0x2a381 | `(path, u64, u64, &out1, &out2, &out3)`. 0 means success; then only `out2` is used, stored shifted left by 16 as a position key. On failure the caller builds a synthetic key with bit 63 set. The two u64 are probably offset and length |
| `lbQKqsERhtE` | libSceFios2.prx 0x2d71c, a virtual method | `(path or NULL, u64, u64, &int)`. Non-zero is failure and latches a module-wide error flag. Otherwise the method returns `*out != 0`: a yes/no query on a file range |

## libSceSystemService, libSceUserService, libSceAppContent

| Library | NID | Behaviour |
|---|---|---|
| libSceSystemService | `fZo48un7LK4` | Integer query by id *(survey)* |
| libSceSystemService | `-+3hY+y8bNo`, `Vo5V8KAwCmk` | Start-up path calls, values from their call sites (#90) *(survey)* |
| libSceUserService | `CdWp0oHWGr0` | Returns a user id *(survey)* |
| libSceUserService | `1xxcMiGu2fo` | Fills a user name *(survey)* |
| libSceUserService | `fPhymKNvK-A` | Start-up path call (#90) *(survey)* |
| libSceAppContent | `99b82IKXpH4` | Takes one of five keys, 0..4 *(survey)* |
| libSceAppContent | `xnd8BJzAxmk` | Returns a count, the add-on content (#90) *(survey)* |

## libScePlayGo (eboot)

All call sites are in one eboot class (0x1fde730–0x1fdec3d) that keeps a handle at +0x10, up to 100 u16 ids at +0x14, one status byte per id at +0xdc and the id count at +0x140.

| NID | Callers | Behaviour |
|---|---|---|
| `73fF1MFU8hA` | 0x1fdeb0f | `(handle, u16 ids[], 100, &count)`: fills up to 100 ids. Result ignored; a count of 0 sets the class's state to 2 |
| `uWIYLFkkwqk` | 0x1fde8d6 (per-frame poll), 0x1fdeb30 (init) | `(handle, ids, count, status bytes[])`: one status byte per id (0, 1 or 2 seen). Non-zero result sets state 2. Otherwise the bytes are folded into a state that starts at 2: per byte, 0 gives 0, 2 gives 1, other values keep the previous state. State 2 means done: init skips the progress callback (`*0x28(vtable)(1)`, 0x1fdeb8f) and the poll stops; any other state makes the poll set +0x160 = 1 every frame (0x1fde89b) |
| `4AAcTU9R3XM` | 0x1fdec3d (tail jump from a setter) | `(handle, mode)` with mode 0, 1 or 2; the caller caches the last mode and skips repeats |
