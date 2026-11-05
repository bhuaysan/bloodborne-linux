// Guest file descriptors and the file functions (docs/design/m3-core-os.md, "Files and paths").
// Guest descriptors are numbers in our own table, so the game never sees, closes or reuses the
// runtime's host descriptors. Other kernel modules (mmap, directories) look descriptors up here.
#pragma once

#include <unistd.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "hle/kernel/abi.hpp"

namespace bb::hle::kernel {

// One open host descriptor, closed when the last reference goes. A call that holds a reference
// keeps a valid host descriptor while another thread closes the guest descriptor.
struct OpenFile {
    explicit OpenFile(int fd) : host_fd(fd) {}
    ~OpenFile() { ::close(host_fd); }
    OpenFile(const OpenFile&) = delete;
    const int host_fd;
};

// Guest descriptor numbers to open files. A new descriptor gets the lowest free number, as in
// POSIX. Thread-safe.
class FdTable {
public:
    // Takes ownership of `host_fd` and returns the lowest free guest number for it.
    int insert(int host_fd);

    // The open file behind `guest`, or nullptr if it is not open.
    std::shared_ptr<OpenFile> get(int guest) const;

    // Frees `guest`. The host descriptor closes once no call uses it any more. False if `guest`
    // was not open.
    bool remove(int guest);

private:
    mutable std::mutex lock_;
    std::vector<std::shared_ptr<OpenFile>> slots_;
};

// The process-wide table that the HLE functions use. On first use, guest 0, 1 and 2 hold
// duplicates (dup) of the host's stdin, stdout and stderr; a host stream that is closed is
// replaced by /dev/null.
FdTable& fd_table();

// A private copy of host stream `host` (0, 1 or 2) for the guest table, numbered 3 or more so
// that a guest close never reaches a host descriptor the runtime reuses. A closed stream gives
// /dev/null. Returns -1 if neither works.
int copy_host_stream(int host);

// ---- the HLE functions, declared for the tests -------------------------------------------------

// sce convention: the result, or an sce error code. Read, write and seek results are 64-bit.
std::int32_t sceKernelOpen(const char* path, std::int32_t flags, std::uint32_t mode);
std::int32_t sceKernelClose(std::int32_t fd);
std::int64_t sceKernelRead(std::int32_t fd, void* buffer, std::uint64_t size);
std::int64_t sceKernelWrite(std::int32_t fd, const void* buffer, std::uint64_t size);
std::int64_t sceKernelPread(std::int32_t fd, void* buffer, std::uint64_t size,
                            std::int64_t offset);
std::int64_t sceKernelPwrite(std::int32_t fd, const void* buffer, std::uint64_t size,
                             std::int64_t offset);
std::int64_t sceKernelLseek(std::int32_t fd, std::int64_t offset, std::int32_t whence);
std::int32_t sceKernelFstat(std::int32_t fd, GuestStat* out);
std::int32_t sceKernelStat(const char* path, GuestStat* out);
std::int32_t sceKernelFsync(std::int32_t fd);

// POSIX convention: 0, or -1 with the guest errno set.
int guest_fstat(std::int32_t fd, GuestStat* out);    // libkernel fstat
int posix_stat(const char* path, GuestStat* out);    // libScePosix stat

}  // namespace bb::hle::kernel
