# Kernel ABI values

The values of the guest kernel ABI (FreeBSD 10, amd64) that differ from Linux or that the runtime must check, and how `src/hle/kernel/abi.hpp` translates them. Every HLE module uses those tables. The struct sections (timespec, timeval, stat, getdirentries record) fix the layouts and how the conversion from the host fills each field. The error conventions per call family are in [m3-core-os.md](../design/m3-core-os.md#errors).

## Sources

Each value names its source:

- **errno(2)**: FreeBSD errno(2), 10.4-RELEASE.
- **header**: FreeBSD's public system headers, release 10.4.0, upstream source tree (allowed by [clean-room.md](../clean-room.md), logged in [references.md](../references.md)). The file is named per section.
- **libc NNNN**, **eboot NNNN**, **Fios NNNN**: a use in `libc.prx`, the eboot or `libSceFios2.prx` of the pinned build, at that link address. The address is where the value appears as a constant or offset. Read from our standard-ELF rewrites with objdump.
- **no use found**: the value comes from the source before it only. Nothing in the three files of the load set was found to use it, so it is not confirmed.

Imports named below are those of `symbols/nids.csv`. One unnamed libkernel import appears: the **errno-location import** `9BcDykPmo1I`, which returns the calling thread's `int*` errno (see [Errno storage](#errno-storage)).

## errno

Numbers 1 to 96 (`ELAST`) from errno(2) and `sys/sys/errno.h`. `EWOULDBLOCK` equals `EAGAIN` and `ENOTSUP` equals `EOPNOTSUPP`, as on Linux. The kernel-internal negative values (`ERESTART` -1 to `EDIRIOCTL` -4) never reach the guest.

Uses that confirm numbers:

| Value | Name | Use |
|---|---|---|
| 1, 2, 13, 28 | EPERM, ENOENT, EACCES, ENOSPC | eboot 0x64c1c–0x64c46 compares errno with these after `fopen`, `ftell`, `fread`, `fwrite`, `fclose` |
| 4 | EINTR | libc 0x61f5: a sleep function repeats `nanosleep` while errno is 4 |
| 5 | EIO | libc 0x275ff: the default errno for an sce code it can't decode (see [sce error codes](#sce-error-codes)) |
| 9 | EBADF | libc 0x73fb |
| 12 | ENOMEM | libc 0xebd4: `fopen` without a free stream |
| 22 | EINVAL | libc 0xed55 (`fopen` with a bad mode), 0xefc9 (`fseek` with a bad argument), 0x83f1 (size overflow) |
| 33, 34 | EDOM, ERANGE | libc 0x29fa3, 0x29fb5: set from the floating-point invalid and overflow/underflow/divide-by-zero flags |
| 34 | ERANGE | libc 0xe005, 0x1165c and 9 more sites |
| 86 | EILSEQ | libc 0x9460: a code point at or above 0x110000 |
| 0x80020000 \| n | most of 1–35, 45, 63, 66, 78 | eboot 0x2073530 groups sce codes into its own file results, consistently with the FreeBSD meanings: 1 and 13 together (permission), 2 alone (not found), 4 and 35 together (`EINTR`, `EAGAIN`: try again), 11, 14 and 16 together, 17 (`EEXIST`), 23, 24, 27, 31 together (limits), 28 (`ENOSPC`), 30 (`EROFS`), 29, 45 and 78 together (`ESPIPE`, `EOPNOTSUPP`, `ENOSYS`: unsupported), 19, 20, 22, 63 together (bad path or argument), 66 (`ENOTEMPTY`) |

**Translation.** `guest_errno(host)` and `host_errno(guest)` map every number that both sides have. Linux's `ENODATA` maps to `ENOATTR` (87): Linux's xattr calls report a missing attribute with `ENODATA` and define `ENOATTR` as its synonym (Linux getxattr(2)). These guest numbers have no Linux error and never come out of a translation: 67 `EPROCLIM`, 72–76 (`EBADRPC`, `ERPCMISMATCH`, `EPROGUNAVAIL`, `EPROGMISMATCH`, `EPROCUNAVAIL`), 79 `EFTYPE`, 80 `EAUTH`, 81 `ENEEDAUTH`, 88 `EDOOFUS`, 93 `ENOTCAPABLE`, 94 `ECAPMODE`. A Linux errno without a guest number (`ENOMEDIUM`, `EKEYEXPIRED`, …) makes `guest_errno` return nothing. The family helpers (`posix_fail`, `pthread_error`, `sce_fail`) turn it into `EIO` and report it on stderr once per value.

## sce error codes

A kernel sce result is 0 or positive on success and `0x80020000 | errno` on failure. Source: libc 0x275ee–0x27604 (the same code follows every libSceFios2 call in libc's file layer, e.g. 0x2782f): libc takes a negative result, and if bits 16–27 are 0x002 it sets errno to the low 16 bits, otherwise to `EIO`. It returns -1. Before that it rewrites libSceFios2's own codes (0x8082xxxx) into kernel codes (`0x80020002`, `0x80020009`, …) through a table at libc 0x27630. The eboot compares a `sceKernelStat` result with `0x80020002` (`ENOENT`) at eboot 0x207457d.

`sce_error(guest_errno)` builds the code, `errno_from_sce(result)` is libc's decoding.

**errno after an sce call.** The eboot reads errno after a failed `sceKernelLseek` (eboot 0x2073d13) and after a failed `sceKernelStat` (eboot 0x20745a5), and passes it to its sce code mapper at eboot 0x2073530. That mapper only recognises codes 0x80020001–0x8002004e and maps every other value, every errno number included, to its generic failure. So the value of errno makes no difference there, and sce entry points leave errno alone, as the design note says.

## Errno storage

libc.prx and the eboot reach errno only through the errno-location import: libc calls it 41 times and the eboot 66 times, and both read and write a 4-byte `int` through the returned pointer (e.g. libc 0x2760c, eboot 0x64c1c). `guest_errno_location()` returns the calling thread's cell, a `thread_local int` that starts at 0. The errno-location import (issue #89) returns exactly that pointer. glibc's `errno` is never handed to the guest.

## open flags

Header `sys/sys/fcntl.h`. Linux values are those of glibc's `<fcntl.h>` on x86-64.

| Guest | Name | Host | Use |
|---|---|---|---|
| 0x0 | O_RDONLY | O_RDONLY (0) | libc 0x8f380 (`fopen` access table), libc 0x53090; Fios 0x2d1b7 |
| 0x1 | O_WRONLY | O_WRONLY (1) | libc 0x8f380, 0x2754d; eboot 0x2073ae1; Fios 0x2d19f |
| 0x2 | O_RDWR | O_RDWR (2) | libc 0x8f380, 0x27572; eboot 0x2073ac3; Fios 0x2d1af |
| 0x3 | O_ACCMODE | (invalid) | reported as unknown |
| 0x4 | O_NONBLOCK | O_NONBLOCK | Fios 0x2d39c: directories are opened with 0x20004 |
| 0x8 | O_APPEND | O_APPEND | libc 0x17ede, 0x27553; eboot 0x2073af1; Fios 0x2d1ba |
| 0x10 | O_SHLOCK | none | no use found; reported as unknown |
| 0x20 | O_EXLOCK | none | no use found; reported as unknown |
| 0x40 | O_ASYNC | O_ASYNC | no use found |
| 0x80 | O_FSYNC, O_SYNC | O_SYNC | no use found |
| 0x100 | O_NOFOLLOW | O_NOFOLLOW | no use found |
| 0x200 | O_CREAT | O_CREAT | libc 0x17ef3, 0x274ee (reads the mode argument), 0x2755e; eboot 0x2073ac3; Fios 0x2d1d5 (mode argument only with it) |
| 0x400 | O_TRUNC | O_TRUNC | libc 0x17eeb, 0x2755e; Fios 0x2d1e2 |
| 0x800 | O_EXCL | O_EXCL | libc 0x17ef9 (`fopen`'s flag conversion has it, its mode parser never sets it) |
| 0x8000 | O_NOCTTY | O_NOCTTY | no use found |
| 0x10000 | O_DIRECT | O_DIRECT | Fios 0x2d1ed (from a libSceFios2 open flag) |
| 0x20000 | O_DIRECTORY | O_DIRECTORY | eboot 0x20723b4; Fios 0x2d39c |
| 0x40000 | O_EXEC | none | no use found; reported as unknown |
| 0x80000 | O_TTY_INIT | none | no use found; reported as unknown |
| 0x100000 | O_CLOEXEC | O_CLOEXEC | no use found |

`fopen`'s mode parser (libc 0xebad–0xec4b) and flag conversion (libc 0x17ed0) give: `"r"` 0x0, `"w"` 0x601, `"a"` 0x209, with `+` the access mode becomes `O_RDWR`. The creation mode is 0666 (libc 0x17f0f), or 0600 for one caller. libc's `open` (libc 0x27470) hands the flags to libSceFios2, which opens files with `sceKernelOpen` (Fios 0x2d203). The eboot also calls `sceKernelOpen` directly, with flags from its own table (eboot 0x2073ab0: 0x0, 0x201, 0x202, plus 0x8).

Going from host to guest, the x86-64 kernel's `__O_LARGEFILE` (0x8000) is dropped. The kernel sets it in every `fcntl(F_GETFL)` result, but glibc defines `O_LARGEFILE` as 0 for 64-bit code, so it can't be matched by name. It is not guest `O_NOCTTY` (host `O_NOCTTY` is 0x100).

`O_DIRECT` translates faithfully, but Linux requires aligned buffers and offsets for it. The file I/O implementation drops it.

## lseek whence

Header `sys/sys/unistd.h`. The values equal Linux's, so `host_whence` only checks the range.

| Value | Name | Use |
|---|---|---|
| 0 | SEEK_SET | eboot 0x20725df (rewinding a directory to its base); libc 0xef48 (`fseek`) |
| 1 | SEEK_CUR | libc 0xe8b2 (`ftell`), 0xef31 |
| 2 | SEEK_END | libc 0xef42 |
| 3 | SEEK_DATA | no use found |
| 4 | SEEK_HOLE | no use found |

## mmap protection and flags

Header `sys/sys/mman.h`. `MAP_FAILED` is -1: libc compares with it after every `mmap` (e.g. libc 0x52fb4).

| Guest | Name | Host | Use |
|---|---|---|---|
| 0x1 | PROT_READ | PROT_READ | libc 0x530d1 (file mapping) |
| 0x2 | PROT_WRITE | PROT_WRITE | libc 0x52f96 (anonymous mappings use 0x3) |
| 0x4 | PROT_EXEC | PROT_EXEC | no use found |
| 0x10, 0x20 | above PROT_EXEC | none | reported as unknown by the table. The memory calls accept them, keep them in the region table and drop them for the host (design note, Memory). The eboot passes 0x33 to `IWIBBdTHit4` (0x2081da6) and tests bit 4 of the protection that `WFcfL2lzido` reports (0x26bae04) |

| Guest | Name | Host | Use |
|---|---|---|---|
| 0x1 | MAP_SHARED | MAP_SHARED | no use found |
| 0x2 | MAP_PRIVATE | MAP_PRIVATE | libc 0x530d6, 0x52f9b |
| 0x10 | MAP_FIXED | MAP_FIXED | no use found |
| 0x20 | MAP_RENAME | none | no use found; reported as unknown |
| 0x40 | MAP_NORESERVE | MAP_NORESERVE | no use found |
| 0x200 | MAP_HASSEMAPHORE | none | no use found; reported as unknown |
| 0x400 | MAP_STACK | MAP_STACK | no use found. Linux treats it as a hint only |
| 0x800 | MAP_NOSYNC | none | no use found; reported as unknown |
| 0x1000 | MAP_ANON | MAP_ANONYMOUS | libc 0x52f9b, 0x54f17, 0x5535f, 0x578b4, 0x57901 (all 0x1002, fd -1) |
| 0x2000 | (MAP_GUARD in later releases) | none | not confirmed in the 10.4 header; reported as unknown |
| 0x4000 | MAP_EXCL | none | no use found; reported as unknown |
| 0x20000 | MAP_NOCORE | none | no use found; reported as unknown |
| 0x40000 | MAP_PREFAULT_READ | none | no use found; reported as unknown |
| 0x80000 | MAP_32BIT | MAP_32BIT | no use found |
| n << 24 | MAP_ALIGNED(n) | none | no use found; reported as unknown |

libc's file mapping is `mmap(0, st_size, PROT_READ, MAP_PRIVATE, fd, 0)` after `fstat` (libc 0x530be–0x530e1). The eboot's only `sceKernelMprotect` call (eboot 0x2903214) passes its caller's value through.

`msync` gets flags 1 at its only call (libc 0x541a2, probing one 4 KiB page). Its flags, same header:

| Guest | Name | Host | Use |
|---|---|---|---|
| 0 | MS_SYNC | MS_SYNC (4) | no use found. Not a bit in the guest: a call without MS_ASYNC is synchronous |
| 0x1 | MS_ASYNC | MS_ASYNC | libc 0x541a2 |
| 0x2 | MS_INVALIDATE | MS_INVALIDATE | no use found |

Other bits are reported as unknown and refused with `EINVAL`. So is MS_ASYNC together with MS_INVALIDATE (3), which the guest kernel's `msync` refuses with `EINVAL` although Linux allows it (FreeBSD [msync(2)](https://man.freebsd.org/cgi/man.cgi?query=msync&sektion=2&manpath=FreeBSD+10.4-RELEASE), 10.4-RELEASE, ERRORS; logged in [references.md](../references.md)).

## Clock ids

Header `sys/sys/time.h`. `clockid_t` is a 32-bit int.

| Guest | Name | Host | Use |
|---|---|---|---|
| 0 | CLOCK_REALTIME | CLOCK_REALTIME | no use found |
| 1 | CLOCK_VIRTUAL | none | no use found; reported as unknown |
| 2 | CLOCK_PROF | none | no use found; reported as unknown |
| 3 | unassigned | none | reported as unknown |
| 4 | CLOCK_MONOTONIC | CLOCK_MONOTONIC | eboot 0xd1a780, libSceFios2.prx 0x2cc4b: the only clock of `QBi7HCK03hw`, read as an elapsed time |
| 5 | CLOCK_UPTIME | CLOCK_MONOTONIC | no use found |
| 6 | unassigned | none | reported as unknown |
| 7 | CLOCK_UPTIME_PRECISE | CLOCK_MONOTONIC | no use found |
| 8 | CLOCK_UPTIME_FAST | CLOCK_MONOTONIC | no use found |
| 9 | CLOCK_REALTIME_PRECISE | CLOCK_REALTIME | no use found |
| 10 | CLOCK_REALTIME_FAST | CLOCK_REALTIME | no use found |
| 11 | CLOCK_MONOTONIC_PRECISE | CLOCK_MONOTONIC | no use found |
| 12 | CLOCK_MONOTONIC_FAST | CLOCK_MONOTONIC | no use found |
| 13 | CLOCK_SECOND | CLOCK_REALTIME | libc 0x2f050: libc's `time` is `clock_gettime(13)` and returns `tv_sec`, so 13 must be the wall clock |
| 14 | CLOCK_THREAD_CPUTIME_ID | CLOCK_THREAD_CPUTIME_ID | no use found |
| 15 | CLOCK_PROCESS_CPUTIME_ID | CLOCK_PROCESS_CPUTIME_ID | no use found |

The eboot doesn't import `clock_gettime` by name. Its clocks are `gettimeofday`, `sceKernelUsleep`, the unnamed libkernel time imports ([kernel-imports.md](kernel-imports.md#time)), and libc's `time` and `clock`. libc's `clock` export (0x2ef60) tail-jumps to `4J2sUJmuHZQ`; an unexported libc function next to it (0x2eef0) computes the same microseconds from `getrusage(0)` (libc 0x2ef11). The mapping of the `_FAST` and `_PRECISE` variants and of `CLOCK_UPTIME` to Linux clocks is our choice (design note, Time: every counter from `CLOCK_MONOTONIC`).

## timespec and timeval

Headers `sys/sys/_timespec.h`, `sys/sys/_timeval.h`, `sys/x86/include/_types.h`. Both are 16 bytes, the same layout as on Linux x86-64.

| Struct | Field | Offset | Type | Use |
|---|---|---|---|---|
| timespec | tv_sec | 0 | int64 | libc 0x2f067 (`time`) |
| timespec | tv_nsec | 8 | int64 | libc 0x61fe (`sleep` passes the remainder on) |
| timeval | tv_sec | 0 | int64 | libc 0x6c82 (`gettimeofday` × 1000000) |
| timeval | tv_usec | 8 | int64 | libc 0x6c8a; libc 0x2ef29 (`clock` adds the two `ru_utime`/`ru_stime` timevals of `struct rusage` at offsets 0 and 16) |

`GuestTimespec` and `GuestTimeval` hold them. `guest_timespec` and `guest_timeval` copy the host fields, `host_timespec` converts back (for a guest `nanosleep` or `clock_settime` argument).

`struct rusage` is 144 bytes: libc's only `getrusage` caller (0x2eef0) reserves 0x90 bytes below its stack guard, passes `who` 0 and reads only the two timevals at offsets 0 (`ru_utime`) and 16 (`ru_stime`). The remaining 112 bytes are taken as 14 8-byte counters, as in the Linux x86-64 layout. `guest_getrusage` (`GuestRusage`) fills the two timevals from the host's `RUSAGE_SELF` and zeroes the 14 counters; any `who` other than 0 (`RUSAGE_SELF`) fails with `EINVAL`.

## struct stat

Header `sys/sys/stat.h` (the layout before 64-bit inode numbers), 120 bytes on amd64. The eboot's `stat` buffer at eboot 0x6542a is 120 bytes below its stack guard.

| Field | Offset | Type | Use |
|---|---|---|---|
| st_dev | 0 | uint32 | no use found; truncated from Linux's 64-bit `dev_t` |
| st_ino | 4 | uint32 | no use found; truncated from Linux's 64-bit `ino_t` |
| st_mode | 8 | uint16 | eboot 0x2072e74 (`& 0444`), 0x2073084 (`& 0222`); Fios 0x2d383 (`& 0xf000 == 0x4000`), 0x989a (`== 0x8000`), 0xa46c (0400, 0200) |
| st_nlink | 10 | uint16 | no use found; clamped to 0xffff |
| st_uid | 12 | uint32 | no use found |
| st_gid | 16 | uint32 | no use found |
| st_rdev | 20 | uint32 | no use found; truncated |
| st_atim | 24 | timespec | eboot 0x20730b9, 0x6543e; Fios 0xa430 (`tv_sec`) |
| st_mtim | 40 | timespec | eboot 0x2073129, 0x65446; Fios 0xa440 (`tv_sec`) |
| st_ctim | 56 | timespec | no use found |
| st_size | 72 | int64 | eboot 0x2074df6, 0x6544d; libc 0x530be; Fios 0xa419 |
| st_blocks | 80 | int64 | no use found |
| st_blksize | 88 | uint32 | eboot 0x20724cf (the buffer size for `sceKernelGetdirentries`) |
| st_flags | 92 | uint32 | no use found; 0 |
| st_gen | 96 | uint32 | no use found; 0 |
| st_lspare | 100 | int32 | 0 |
| st_birthtim | 104 | timespec | no use found; 0, Linux's `stat` has no birth time |

The file type bits (`S_IFMT` 0170000, `S_IFIFO` 0010000, `S_IFCHR` 0020000, `S_IFDIR` 0040000, `S_IFBLK` 0060000, `S_IFREG` 0100000, `S_IFLNK` 0120000, `S_IFSOCK` 0140000) and permission bits are the same as on Linux, so `st_mode` is copied. A `static_assert` in `abi.cpp` checks the type bits against the host headers.

`GuestStat` is the guest layout, `guest_stat(host)` fills it from a Linux `struct stat` as the table says: the wider host fields are truncated, `st_nlink` is clamped, `st_flags`, `st_gen`, `st_lspare` and `st_birthtim` are 0.

## getdirentries record

Header `sys/sys/dirent.h`. `sceKernelGetdirentries(fd, buf, nbytes, basep)` fills `buf` with records. `basep` is a pointer to a 64-bit offset (eboot 0x20725a6), or null (Fios 0x2d425).

| Field | `GuestDirent` | Offset | Type | Use |
|---|---|---|---|---|
| d_fileno | fileno | 0 | uint32 | Fios 0x2d453 skips records with 0 |
| d_reclen | reclen | 4 | uint16 | eboot 0x2072587; Fios 0x2d447 (the step to the next record) |
| d_type | type | 6 | uint8 | no use found |
| d_namlen | namlen | 7 | uint8 | no use found |
| d_name | name | 8 | char[256] | eboot 0x2072560, Fios 0x2d45a (compared with `.` and `..`) |

A full record is 264 bytes (Fios 0x2d475 copies 0x108 bytes per entry). A record's length is `8 + ((d_namlen + 1 + 3) & ~3)`: the name is NUL-terminated and padded with NULs to a multiple of 4 (header). The `d_type` values (`DT_UNKNOWN` 0, `DT_FIFO` 1, `DT_CHR` 2, `DT_DIR` 4, `DT_BLK` 6, `DT_REG` 8, `DT_LNK` 10, `DT_SOCK` 12, `DT_WHT` 14) are the same as on Linux. A `static_assert` in `abi.cpp` checks them, so a host `d_type` is passed through.

`GuestDirent` names its fields without the `d_` prefix because glibc's `<dirent.h>` defines `d_fileno` as a macro. `dirent_length(n)` is the record length for an `n`-byte name. `put_dirent(out, fileno, type, name)` writes one record to the start of `out` with `memcpy`, since a guest buffer need not be aligned, and returns its length. It writes nothing and returns 0 if the record doesn't fit or the name is empty or longer than 255 bytes. Bytes after the record's padding are left alone.

## Event queue

Three unnamed libkernel imports (see [kernel-imports.md](kernel-imports.md#counted-sync-object-and-event-queue)), implemented in `src/hle/kernel/equeue.cpp`. The eboot calls create twice, delete six times and wait five times; libc.prx and libSceFios2.prx don't import them.

- **Create** `D0OdFMjp46I` `(uint64* out, const char* name)`: writes a handle to `*out` and returns 0. The handle is a 64-bit value (eboot 0x11e4899 keeps it in a qword); 0 means "no queue" to the game (eboot 0x26d633b skips the delete when it is 0), so ours start at 1. The names are `"FlipEventQueue"` (eboot 0x26d6202) and `"Initialize"` (eboot 0x11e48a0). A negative result is a failure (eboot 0x26d6211). After each create the game passes the handle to a video out call (eboot 0x11e48bb, 0x26d6234), with 0 as the third argument: the flip event registration (#100).
- **Delete** `jpFjmgAC5AE` `(uint64 handle)`: returns 0. The game ignores the result (eboot 0x26d6347).
- **Wait** `fzyMKs9kim0` `(uint64 handle, event* events, int32 capacity, int32* count, uint32* timeout)`: every call site passes capacity 1, a 32-byte stack buffer (eboot 0x11e41a6: the buffer at `rbp-0x30` ends at the stack guard at `rbp-0x10`; eboot 0x26d6f1b: `rbp-0x98` to the count at `rbp-0x74`), a 4-byte count and a null timeout. eboot 0x26d6f2b leaves its loop on a negative result; the other four ignore it.

No call site reads a field of the event record, so its layout is ours (`GuestEvent`, 32 bytes, the size of the game's buffer):

| Field | Offset | Type | Meaning |
|---|---|---|---|
| id | 0 | uint64 | event source, chosen by the producer |
| kind | 8 | int16 | event type, chosen by the producer |
| flags | 10 | uint16 | 0 so far |
| detail | 12 | uint32 | producer-defined |
| data | 16 | int64 | producer-defined |
| user_data | 24 | uint64 | the value the guest gave when it registered for the event |

The timeout's type is not shown by any event-queue call site. The game's counted-sync wait passes its timeout the same way, as a pointer to a 4-byte value, and null for -1 (eboot 0x263ae41), so the wait reads a `uint32` in microseconds (the unit is our choice). It does not write the remaining time back.

**Results** (our choices, `sce_error` codes as in [sce error codes](#sce-error-codes)):

| Case | Result | `*count` |
|---|---|---|
| events returned | 0 | number written, at most `min(capacity, 64)` |
| timeout passed with nothing pending | `0x8002003c` (`ETIMEDOUT`) | 0 |
| queue deleted while waiting | `0x80020055` (`ECANCELED`) | 0 |
| unknown or deleted handle (wait, delete) | `0x80020009` (`EBADF`) | unchanged |
| null `out`, `events` or `count`, capacity < 1 | `0x80020016` (`EINVAL`) | unchanged |

**Producers** (our own HLE modules) call `bb::hle::EventQueue::trigger(handle, event)`. It returns false and drops the event if the handle is not a live queue. Events come out in trigger order. A trigger whose `(id, kind)` is already pending replaces that record in place, so a producer that fires faster than the game waits doesn't grow the queue. A trigger wakes one waiter; a wait that leaves events behind wakes the next.

## sysctl MIB

Header `sys/sys/sysctl.h`. libc.prx passes one MIB to `sysctl` (`DFmMT80xcNI`), from the function at 0x52ee0: four `int`s, the last the process id. It calls once for the size (0x52f50), then for the records (0x52fe1). Every other MIB is refused (`src/hle/kernel/process.cpp`).

| Level | Name | Value | Use |
|---|---|---|---|
| 0 | `CTL_KERN` | 1 | libc 0x52f20 |
| 1 | `KERN_PROC` | 14 | libc 0x52f27 |
| 2 | `KERN_PROC_VMMAP` | 32 | libc 0x52f2e |

`kern.proc.vmmap` returns the memory map of a process as variable-length records. libc reads the record length at +0 and compares the start and end address at +8 and +0x10 with the address it looks for (0x53040, 0x53049). The runtime reports an empty map (length 0), so the record layout needs no translation.

## Path limits

Header `sys/sys/syslimits.h`. The path resolver (`src/hle/kernel/paths.cpp`) checks both before it maps a path.

| Value | Name | Meaning | Use |
|---|---|---|---|
| 1024 | PATH_MAX | bytes in a path, including the NUL: a path of 1024 bytes or more fails with ENAMETOOLONG | no use found |
| 255 | NAME_MAX | bytes in one path component, without the NUL: a longer component fails with ENAMETOOLONG | no use found; it matches `d_name`'s 256 bytes in the [getdirentries record](#getdirentries-record) |

## Mutexes and rwlocks

The libkernel mutex, mutex attribute and rwlock imports and the libScePosix `pthread_mutex*` imports (issue #86), implemented in `src/hle/kernel/sync.cpp`. libScePosix imports no rwlock function. The values below are from the call sites of the three files of the load set. Where a value has no use, its meaning is our assumption and says so.

**Object size.** A mutex, a mutex attribute and an rwlock are each one 8-byte cell:

| Object | Use |
|---|---|
| mutex | libc 0x64da (`+0x8` of libc's 0x28-byte lock object, the condition variable that libc waits on with it follows at `+0x10`, 0x6630); libc bss cells 0xc28f0 and 0xc28f8, two mutexes 8 bytes apart (0x52980, 0x53f90); Fios 0x2e558 (`+0x0` of its wrapper, owner at `+0x8`, 0x2e505) |
| mutex attribute | libc 0xf355 (`rbp-0x58`, the name buffer starts at `rbp-0x50`); Fios 0x2e515 (`rbp-0x30`, the stack guard at `rbp-0x28`); eboot static attribute 0x53e9928 with a flag byte at 0x53e9930 (0x115d0) |
| rwlock | eboot 0x207f401 (`+0x8` of its wrapper object); Fios 0x2e720 (the wrapper's own pointer is passed through) |

The mutex and rwlock cells hold a pointer to our object (a tag and the glibc primitive). The attribute cell holds the values themselves, tagged in its upper half, so an attribute needs no object of ours and its destruction frees nothing.

**Static initialiser.** 0, for mutexes: libc locks bss cells that nothing initialises (0xc28f0 at 0x52980, 0xc28f8 at 0x53f90, 0xcd3e0 at 0x57752, 0xd5c70 at 0x600e8), and so does the eboot through `pthread_mutex_lock` (bss 0x54b91e0 at 0x29f35ed). No mutex cell with another initial value was found, and every rwlock that the eboot and libSceFios2 use is initialised first (eboot 0x207f2d3, Fios 0x2e72d). A cell holding 0 gets an object of the default type on first use, created under a global lock. Destroying a mutex or rwlock resets its cell to 0. Any other value that is not one of our objects is refused with `EINVAL` (values below 0x10000 without reading through them).

**Initialisation.** `scePthreadMutexInit(mutex, attr, name)` and `scePthreadRwlockInit(rwlock, attr, name)` take a name as a third argument: libc passes a formatted buffer (0x64e1), the eboot passes null (0xd1ac41, and 0x207f2d1 for an rwlock). The name is not kept. `attr` may be null, which means the default attributes (libc 0x64e5, Fios 0x2e72b for an rwlock). No rwlock attribute function is imported, so the rwlock `attr` is ignored.

**Types.** The type setter's second argument:

| Value | Type | Host | Use |
|---|---|---|---|
| 1 | error-checking | `PTHREAD_MUTEX_ERRORCHECK` | no use found |
| 2 | recursive | `PTHREAD_MUTEX_RECURSIVE` | Fios 0x2e52b: the wrapper picks 2 when its recursion flag is 1, and then starts its own count at 0 instead of -1 (0x2e509); eboot 0xd1ac2f, 0x208400a and all 39 `pthread_mutexattr_settype` calls (e.g. 0x110ba); libc 0xf36b |
| 3 | normal | `PTHREAD_MUTEX_NORMAL` | no use found |
| 4 | adaptive | `PTHREAD_MUTEX_ERRORCHECK` | Fios 0x2e52b, its non-recursive mutexes |

The default type (attribute just initialised, null attribute, static initialiser) is error-checking: libc's lock at 0x65e1 tells `EDEADLK` (0x8002000b) apart from other failures on a mutex made with a null attribute (0x64e7). The eboot's wrapper prints "Deadlock. Mutex is locked from owner thread.(EDEADLK)" on that result (0x2084130). The adaptive type spins before it sleeps; here it is error-checking, which suits its only user, whose non-recursive wrapper never relocks. The names of 1 to 4 and the default (`PTHREAD_MUTEX_DEFAULT` is `PTHREAD_MUTEX_ERRORCHECK`) follow FreeBSD 10.4 `include/pthread.h`, which agrees with the uses above; nothing in the load set uses 1 or 3. Other values are refused with `EINVAL`.

**Protocol.** `scePthreadMutexattrSetprotocol` takes 0, 1 or 2 (`PTHREAD_PRIO_NONE`, `_INHERIT`, `_PROTECT` in the same header); libSceFios2 passes 1 (0x2e540). The value is accepted and not applied (issue #86, out of scope: priority inheritance). Others give `EINVAL`.

**Timeouts.** The timed variants (`scePthreadMutexTimedlock`, `scePthreadRwlockTimedrdlock`, `scePthreadRwlockTimedwrlock`) take a 32-bit timeout as their second argument (eboot 0x2084114, 0x207f408, 0x207f678), which the wrappers pass through unchanged. Every caller of the eboot's wrappers that passes a constant passes -1, and for -1 the wrappers call the untimed variant instead (0x2084117, 0x207f40c, 0x207f67c). The remaining callers reach them through virtual calls. So no value reaches a timed import in the paths we read, and the unit is not confirmed. We take microseconds, relative, like `sceKernelUsleep`, and wait with glibc's `pthread_mutex_clocklock` and `pthread_rwlock_clock{rd,wr}lock` against a deadline on `CLOCK_MONOTONIC`. A timeout gives `ETIMEDOUT`, which the eboot compares as 0x8002003c (0x2084150).

**Results.** The sce entry points return `0x80020000 | errno`, the POSIX ones the errno number. The eboot and libraries compare: `EDEADLK` (libc 0x65ea, eboot 0x2084130, 0x207f421), `EBUSY` from trylock and destroy (libc 0x66fb, Fios 0x2e5e3, 0x2e7ae, eboot 0x20840be), `EINVAL` (eboot 0x2084137), `EAGAIN` for too many recursive locks (eboot 0x208413e), `ENOMEM` from init (eboot 0x2084037). `EPERM` comes from unlocking a mutex the thread doesn't own (error-checking and recursive types). A held rwlock is not destroyed and gives `EBUSY`, as a mutex does: glibc destroys a held rwlock without complaint (and a later unlock then hangs), so the destroy first tries a write lock itself. Unlocking an rwlock that isn't held is not detected.

## Condition variables

The libkernel `scePthreadCond*` imports (libc.prx, libSceFios2.prx and the eboot's two waits) and the libScePosix `pthread_cond_*` imports (the eboot), implemented in `src/hle/kernel/sync.cpp` (issue #95). No condition variable attribute function is imported.

**Object size and initialiser.** One 8-byte cell holding a pointer to our object, as for a mutex. libc's lock object keeps its condition variable at `+0x10`, 8 bytes after its mutex (libc 0x64b7); the eboot allocates a 0x10-byte pair of mutex and condition variable (0x65df7, condition variable at `+0x8`, 0x65e0e). libc waits on a bss cell that nothing initialises (0xd5c90 at libc 0x60158, with the bss mutex 0xd5c70), so 0 is the static initialiser and gets an object on first use, as for mutexes. Destroying one resets its cell to 0.

**Initialisation.** `scePthreadCondInit(cond, attr, name)`: libc passes a formatted name buffer (0x5f3e), libSceFios2 a constant name or none (0x2e0e1, 0x2e0fa); the name is not kept. Every caller of either init passes a null `attr` (libc 0x5f36, 0x64bf; Fios 0x2e0df, 0x2e0f8; eboot 0x65bc4, 0x65e12, 0x6662a, 0x207e6d5, 0x2083d64), and `attr` is ignored.

**Timeouts.**

| Call | Timeout | Use |
|---|---|---|
| `scePthreadCondTimedwait(cond, mutex, usec)` | `uint32`, relative, microseconds | libc 0x6e50 converts an absolute `timespec` to the microseconds left from `gettimeofday`, rounded up, for its call at 0x6029; libSceFios2 computes a deadline minus now in microseconds, × 1000 then +999 / 1000 (0x2e1e4–0x2e218), for 0x2e220. The eboot's wrapper passes its argument through and calls the untimed wait for -1 (0x2083eb8) |
| `pthread_cond_timedwait(cond, mutex, abstime)` | `struct timespec*`, absolute, wall clock | eboot 0x65c91: `gettimeofday`, plus a timeout in milliseconds (÷ 1000 for seconds, remainder × 1000 + `tv_usec`, × 1000 for nanoseconds), for 0x65cf1 |

The sce wait runs `pthread_cond_clockwait` against a `CLOCK_MONOTONIC` deadline, the POSIX wait against the `abstime` on `CLOCK_REALTIME`. A null `abstime` or a `tv_nsec` outside [0, 10⁹) gives `EINVAL`.

**Results.** A timeout gives `ETIMEDOUT` (libc 0x602e and Fios 0x2e22f compare 0x8002003c; the eboot's wrapper turns it into -3, 0x2083f20). The eboot's wrapper also tells `EPERM` (0x80020001, 0x2083ed1) and `EINVAL` (0x2083edd) apart: waiting on a mutex the thread doesn't hold gives `EPERM` for the error-checking and recursive types. Destroying a condition variable that a thread is blocked on gives `EBUSY` (the eboot's wrapper compares 0x10, 0x2083e02; libSceFios2 traps on any failure, 0x2e120); glibc's destroy would block until the waiters leave instead. A waiter that a signal or broadcast has already released doesn't count, even while it waits to take the mutex back: the common teardown "lock, set a flag, broadcast, destroy" succeeds, and destroy waits for the released waiters to leave, as glibc does. To know which waiters are still blocked, each waiter sleeps on its own host condition variable, and signal and broadcast release them in arrival order. A cell that holds no object of ours gives `EINVAL`.

## Counted sync object

Five unnamed libkernel imports, implemented in `src/hle/kernel/counted_sync.cpp` (issue #95). Only the eboot imports them. It creates them in 25 places: 22 binary ones for a mutex that spins before it waits (attribute 2, initial 0, maximum 1, empty name, e.g. 0x1fc243e; its wait 0x2036983 and post 0x2036aaf), a general wrapper with attribute 1 (create 0x263acf1, wait 0x263ae65, try-wait 0x263afc2, post 0x263b082 and 0x263b104) and two thin ones with attribute 1 (0x44ac20 with the name `"hkSemaphore"`, 0xd1ad10 with `"FMOD Semaphore"` and maximum 0xffff). Results are sce codes; the error comparisons below are the general wrapper's.

| NID | Arguments | Behaviour |
|---|---|---|
| `188x57JYp0g` | `(uint64* out, const char* name, int32 attribute, int32 initial, int32 max, const void* options)` | Writes an 8-byte handle (every caller keeps it in a qword and passes it on as one, 0xd1ad57, 0x2036978). Ours start at 1. `attribute` is 1 or 2 (every call site); the eboot's message for `EINVAL` here names an invalid attribute (0x263ad1e). What the two values mean is unknown; both serve waiters in the order of their requests here. `options` is null at every call site. `EINVAL` for a null `out`, another `attribute`, `max` < 1, `initial` < 0 or `initial` > `max` (compared at 0x263ad1e); `ENAMETOOLONG` for a name over 31 bytes (our limit, compared at 0x263ad4f). Names seen are empty or short |
| `R1Jvn8bSCW8` | `(uint64 handle)` | Removes the object. Waiting threads return `EACCES` ("deleted", 0x263af42). `ESRCH` for an unknown handle |
| `Zxa0VhQVTsk` | `(uint64 handle, int32 need, uint32* timeout)` | Takes `need` from the count, waiting while it is short. `timeout` points to a 4-byte value, null for -1 (0x263ae41); we read it as relative microseconds on `CLOCK_MONOTONIC`, like the sce mutex and condition variable timeouts, and don't write the remaining time back (the caller doesn't read it). `ETIMEDOUT` when it passes (0x263af34, the wrapper returns -3), `EINVAL` for `need` < 1 or above the maximum (0x263af50), `ESRCH` for an unknown handle (0x263ae8d) |
| `12wOHk8ywb0` | `(uint64 handle, int32 need)` | The same without waiting: `EBUSY` if the count is short (0x263afd2, the wrapper returns -2), `EINVAL`, `ESRCH` as for the wait |
| `4czppHBiriw` | `(uint64 handle, int32 count)` | Adds `count` and grants waiting requests. `EINVAL` if `count` < 1 or the sum would pass the maximum; the count is then unchanged. The wrapper treats `EINVAL` like success (0x263b08b), which fits a post to a full binary semaphore. `ESRCH` for an unknown handle |

Requests are served strictly in order: while the oldest waiting request is larger than the count, later ones wait too, and the try-wait gives `EBUSY`. A wait that times out leaves the queue, which can let the next requests through.

## Threads

The libkernel thread and thread attribute imports, `sched_yield`, and the libScePosix `pthread_*` thread imports (issue #94). `src/hle/kernel/thread.cpp` implements `scePthreadCreate`, `scePthreadJoin`, `scePthreadExit` and the attribute's init, destroy and stack size so far. Detach, Self, Equal, Yield, `sched_yield`, the other attribute setters, `scePthreadAttrGet` and the libScePosix entry points follow in a second part and stay logging stubs until then. Affinity, names, priority changes, thread-specific data and once are #99.

**Creation.** `scePthreadCreate(&thread, attr, entry, arg, name)` (eboot 0x263b3de, 0xd1ab29; libc 0x60df, 0x634d; Fios 0x2eb4e), `pthread_create(&thread, attr, entry, arg)` (eboot 0x66674, 0x666df) and the unnamed libScePosix `Jmi+9w9u0E4(&thread, attr, entry, arg, name)` (eboot 0x4388aa). For the last one, the eboot's wrapper at 0x438840 passes its own entry, argument and name through, and its callers pass a code address as the entry (0x42326e, used at 0x4232c9); it reports a non-zero result through `perror` (0x4388fb), so the result is an errno number. The name may be null; libc passes a formatted buffer (0x60cc), libSceFios2 at most 15 bytes (0x2eb10).

**Handles.** The handle is 8 bytes (Fios reads it back at 0x2eb55, the eboot stores it at 0x4388c1) and is a pointer to our `Thread` object. The runtime keeps a table of live threads and refuses a handle that isn't in it with `ESRCH`, without reading through it. Create stores the handle and enters the thread in the table before the thread starts, as glibc's `pthread_create` stores its `pthread_t`, so the entry can read its handle variable or pass its own handle on at once; a failed create takes it out again and restores the handle variable. A joined thread leaves the table, and so does a detached thread when it ends. Self returns the handle the creator got (Fios's entry stores it at 0x2ebb3). `scePthreadEqual` returns 1 for the same handle, else 0 (libc 0x6250 passes the result through, Fios 0x2ec19 tests it for non-zero).

**Attributes.** One 8-byte cell (eboot 0x263b36f: at -0x58, below the name buffer at -0x50; eboot 0x438876: at -0x40, below the handle at -0x38) that holds a pointer to our attribute object. Init allocates the object, destroy frees it and resets the cell to 0, and a cell without one of our objects gives `EINVAL`. The eboot initialises one static attribute (0x53e9948) once and passes it to every create at 0x66674. A null attribute means the defaults (libc 0x6345). Defaults: stack 0x10000, joinable, policy, priority and inheritance 0.

| Setter | Values | Applied | Source |
|---|---|---|---|
| stack size | at least 0x4000, else `EINVAL` | yes | eboot's wrapper 0x263b310: 0x10000 when its caller passes 0 (0x263b331), a retry with 0x4000 when the size is refused (0x263b387); eboot 0x66557: 0x20000. `PTHREAD_STACK_MIN` is 16384 in Linux pthread_attr_setstacksize(3) |
| detach state | 0 joinable, 1 detached, else `EINVAL` | yes | 0: eboot 0x263b3b3, 0x43887b, Fios 0x2ea7c (joined at 0x2ebde). 1: eboot 0x6654a, 0x66689, 0xd1aad5 (never joined) |
| sched param | pointer to one `int`, the priority | recorded | eboot 0x66564 (700), 0x666bb (from a table), 0xd1aaf5; Fios 0x2ea99 |
| sched policy | any `int` | recorded | eboot 0x263b3bd, 0xd1aae7: 1 |
| inherit sched | any `int` | recorded | Fios 0x2eaa0: 0 |

The host stack is the requested size plus a 64 KiB reserve for the runtime's own frames (design note, Threads). The setter takes any size from 0x4000 up; a size the host can't give, including one that doesn't fit in 64 bits with the reserve, fails the create with `EAGAIN`. `scePthreadAttrGet(thread, attr)` fills an initialised attribute with the thread's attributes and its current detach state (libc 0x81abf, with Self as the thread).

**Exit.** `scePthreadExit(value)` and `pthread_exit(value)` end the calling thread with `value` as its result. The eboot's thread entry 0x263b1e0 never returns through its `ret`: it ends in 0x207f990, which calls `scePthreadExit` (0x207f99b). libc's wrapper (0x6190) and the eboot (0x667e0) pass an `int` sign-extended to 64 bits.

**Join.** `scePthreadJoin(thread, &value)` and `pthread_join`. `value` may be null (Fios 0x2ebdc). libc reads the low 32 bits of the result (0x6147).

**Results.** The sce entry points return `0x80020000 | errno`, the POSIX ones the errno number. The eboot's wrapper tells `EPERM` (0x80020001), `EINVAL` (0x80020016) and `EAGAIN` (0x80020023) apart after a failed create (0x263b3f3–0x263b405). Following Linux pthread_join(3) and pthread_detach(3): `ESRCH` for a handle not in the table (null included), `EDEADLK` for joining the calling thread, `EINVAL` for joining or detaching a thread that is detached, already being joined, or wasn't started by the runtime. `EINVAL` also for a null handle pointer or entry and for an invalid attribute; `EAGAIN` when the host can't create the thread. `scePthreadYield` returns nothing (libc's wrapper 0x6240 is a tail jump); `sched_yield` returns 0, or -1 with the guest errno.
