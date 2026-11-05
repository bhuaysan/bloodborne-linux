# Kernel ABI values

The values of the guest kernel ABI (FreeBSD 10, amd64) that differ from Linux or that the runtime must check, and how `src/hle/kernel/abi.hpp` translates them. Every HLE module uses those tables. The struct sections (timespec, timeval, stat, getdirentries record) fix the layouts and how the conversion from the host fills each field; the conversion functions themselves are the second part of issue #80. The error conventions per call family are in [m3-core-os.md](../design/m3-core-os.md#errors).

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

`O_DIRECT` translates faithfully, but Linux requires aligned buffers and offsets for it. The file I/O implementation has to drop it.

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
| 0x10, 0x20 | above PROT_EXEC | none | reported as unknown. The design note's direct-memory mappings pass them with read and write *(survey)*; that issue decides how to accept them |

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

## Clock ids

Header `sys/sys/time.h`. `clockid_t` is a 32-bit int.

| Guest | Name | Host | Use |
|---|---|---|---|
| 0 | CLOCK_REALTIME | CLOCK_REALTIME | no use found |
| 1 | CLOCK_VIRTUAL | none | no use found; reported as unknown |
| 2 | CLOCK_PROF | none | no use found; reported as unknown |
| 3 | unassigned | none | reported as unknown |
| 4 | CLOCK_MONOTONIC | CLOCK_MONOTONIC | no use found |
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

The eboot doesn't import `clock_gettime` by name. Its clocks are `gettimeofday`, `sceKernelUsleep`, and libc's `time` and `clock`. libc's `clock` uses `getrusage(0)` (libc 0x2ef11). The mapping of the `_FAST` and `_PRECISE` variants and of `CLOCK_UPTIME` to Linux clocks is our choice (design note, Time: every counter from `CLOCK_MONOTONIC`).

## timespec and timeval

Headers `sys/sys/_timespec.h`, `sys/sys/_timeval.h`, `sys/x86/include/_types.h`. Both are 16 bytes, the same layout as on Linux x86-64.

| Struct | Field | Offset | Type | Use |
|---|---|---|---|---|
| timespec | tv_sec | 0 | int64 | libc 0x2f067 (`time`) |
| timespec | tv_nsec | 8 | int64 | libc 0x61fe (`sleep` passes the remainder on) |
| timeval | tv_sec | 0 | int64 | libc 0x6c82 (`gettimeofday` × 1000000) |
| timeval | tv_usec | 8 | int64 | libc 0x6c8a; libc 0x2ef29 (`clock` adds the two `ru_utime`/`ru_stime` timevals of `struct rusage` at offsets 0 and 16) |

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

The file type bits (`S_IFMT` 0170000, `S_IFDIR` 0040000, `S_IFREG` 0100000, …) and permission bits are the same as on Linux, so `st_mode` is copied.

## getdirentries record

Header `sys/sys/dirent.h`. `sceKernelGetdirentries(fd, buf, nbytes, basep)` fills `buf` with records. `basep` is a pointer to a 64-bit offset (eboot 0x20725a6), or null (Fios 0x2d425).

| Field | Offset | Type | Use |
|---|---|---|---|
| d_fileno | 0 | uint32 | Fios 0x2d453 skips records with 0 |
| d_reclen | 4 | uint16 | eboot 0x2072587; Fios 0x2d447 (the step to the next record) |
| d_type | 6 | uint8 | no use found |
| d_namlen | 7 | uint8 | no use found |
| d_name | 8 | char[256] | eboot 0x2072560, Fios 0x2d45a (compared with `.` and `..`) |

A full record is 264 bytes (Fios 0x2d475 copies 0x108 bytes per entry). A record's length is `8 + ((d_namlen + 1 + 3) & ~3)`: the name is NUL-terminated and padded with NULs to a multiple of 4 (header). The `d_type` values (`DT_UNKNOWN` 0, `DT_FIFO` 1, `DT_CHR` 2, `DT_DIR` 4, `DT_BLK` 6, `DT_REG` 8, `DT_LNK` 10, `DT_SOCK` 12, `DT_WHT` 14) are the same as on Linux.
