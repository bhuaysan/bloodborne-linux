// Directory and metadata calls: mkdir, rmdir, rename, unlink, truncate and getdirentries
// (docs/design/m3-core-os.md, "Files and paths"). Paths go through the mount table, descriptors
// through the descriptor table of file.hpp.
#pragma once

#include <cstdint>

namespace bb::hle::kernel {

// sce convention: 0 (or the byte count for getdirentries), or an sce error code.
std::int32_t sceKernelMkdir(const char* path, std::uint32_t mode);
std::int32_t sceKernelRmdir(const char* path);
std::int32_t sceKernelUnlink(const char* path);
std::int32_t sceKernelRename(const char* from, const char* to);
std::int32_t sceKernelTruncate(const char* path, std::int64_t length);
std::int32_t sceKernelFtruncate(std::int32_t fd, std::int64_t length);

// Fills `buffer` with whole guest records (abi.hpp, GuestDirent) from the read position of
// directory `fd` and returns their total length, 0 at the end of the directory. Junk files are
// skipped. `*base`, if not null, gets the read position before the call; seeking `fd` back to it
// lists the same records again. EINVAL if the next record doesn't fit into `size` bytes.
std::int32_t sceKernelGetdirentries(std::int32_t fd, void* buffer, std::int32_t size,
                                    std::int64_t* base);

// POSIX convention (libScePosix): 0, or -1 with the guest errno set.
int posix_mkdir(const char* path, std::uint32_t mode);
int posix_rmdir(const char* path);
int posix_rename(const char* from, const char* to);

}  // namespace bb::hle::kernel
