#include "hle/kernel/file.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>

#include "common/log.hpp"
#include "hle/hle.hpp"
#include "hle/kernel/paths.hpp"

namespace bb::hle::kernel {
namespace {

constexpr std::string_view channel = "hle.libkernel";

// A result, or a host errno.
struct Outcome {
    std::int64_t value = 0;
    int error = 0;
};

Outcome from_host(std::int64_t result) {
    return result < 0 ? Outcome{0, errno} : Outcome{result, 0};
}

std::int64_t sce(Outcome o) {
    return o.error != 0 ? sce_fail(o.error) : o.value;
}
std::int32_t sce32(Outcome o) {
    return static_cast<std::int32_t>(sce(o));
}

int posix(Outcome o) {
    return o.error != 0 ? posix_fail(o.error) : static_cast<int>(o.value);
}

// Runs `f` on the host descriptor of `fd`, or fails with EBADF.
template <class F>
Outcome with_fd(std::int32_t fd, F f) {
    const std::shared_ptr<OpenFile> file = fd_table().get(fd);
    if (!file) {
        return {0, EBADF};
    }
    return f(file->host_fd);
}

// The host path of `path` in `out`, or the host errno.
int resolve(const char* path, Access access, Resolved& out) {
    if (path == nullptr) {
        return EFAULT;
    }
    out = resolve_path(path, access);
    return out ? 0 : host_errno(out.error).value_or(EIO);
}

Outcome open_file(const char* path, std::int32_t guest_flags, std::uint32_t mode) {
    const Flags flags = host_open_flags(guest_flags);
    if (flags.unknown != 0) {
        common::log(common::LogLevel::warn, channel, "open: unknown flags {:#x}", flags.unknown);
        return {0, EINVAL};
    }
    // Linux needs aligned buffers and offsets for O_DIRECT, which libSceFios2 doesn't promise
    // (docs/formats/kernel-abi.md).
    const int host_flags = (flags.value & ~O_DIRECT) | O_CLOEXEC;
    const bool writes = (host_flags & O_ACCMODE) != O_RDONLY || (host_flags & (O_CREAT | O_TRUNC));
    Resolved r;
    Outcome o{0, resolve(path, writes ? Access::write : Access::read, r)};
    if (o.error == 0) {
        o = from_host(::open(r.host.c_str(), host_flags, static_cast<mode_t>(mode & 07777)));
    }
    if (o.error == 0) {
        o.value = fd_table().insert(static_cast<int>(o.value));
    }
    common::log(common::LogLevel::debug, channel, "open '{}' flags {:#x}: fd {}, host errno {}",
                path != nullptr ? path : "(null)", guest_flags, o.value, o.error);
    return o;
}

// Converts the result of a host stat call into `out`.
Outcome put_stat(int result, const struct stat& st, GuestStat* out) {
    if (result != 0) {
        return {0, errno};
    }
    if (out == nullptr) {
        return {0, EFAULT};
    }
    *out = guest_stat(st);
    return {};
}

Outcome fstat_file(std::int32_t fd, GuestStat* out) {
    return with_fd(fd, [&](int h) {
        struct stat st {};
        return put_stat(::fstat(h, &st), st, out);
    });
}

Outcome stat_path(const char* path, GuestStat* out) {
    Resolved r;
    if (const int error = resolve(path, Access::read, r)) {
        return {0, error};
    }
    struct stat st {};
    return put_stat(::stat(r.host.c_str(), &st), st, out);
}

}  // namespace

int FdTable::insert(int host_fd) {
    auto file = std::make_shared<OpenFile>(host_fd);
    const std::lock_guard guard(lock_);
    auto slot = std::find(slots_.begin(), slots_.end(), nullptr);
    if (slot == slots_.end()) {
        slot = slots_.insert(slot, nullptr);
    }
    *slot = std::move(file);
    return static_cast<int>(slot - slots_.begin());
}

std::shared_ptr<OpenFile> FdTable::get(int guest) const {
    const std::lock_guard guard(lock_);
    if (guest < 0 || static_cast<std::size_t>(guest) >= slots_.size()) {
        return nullptr;
    }
    return slots_[static_cast<std::size_t>(guest)];
}

bool FdTable::remove(int guest) {
    std::shared_ptr<OpenFile> file;  // released after the lock, so a host close never holds it
    const std::lock_guard guard(lock_);
    if (guest < 0 || static_cast<std::size_t>(guest) >= slots_.size()) {
        return false;
    }
    file = std::move(slots_[static_cast<std::size_t>(guest)]);
    return file != nullptr;
}

int copy_host_stream(int host) {
    const int copy = ::fcntl(host, F_DUPFD_CLOEXEC, 3);
    if (copy >= 0) {
        return copy;
    }
    // A closed host stream becomes /dev/null, so that the numbering holds. A plain open
    // would take the lowest free number, which may be the closed stream itself.
    const int null = ::open("/dev/null", O_RDWR | O_CLOEXEC);
    if (null < 0 || null >= 3) {
        return null;
    }
    const int moved = ::fcntl(null, F_DUPFD_CLOEXEC, 3);
    ::close(null);
    return moved;
}

FdTable& fd_table() {
    static FdTable* const table = [] {
        auto* t = new FdTable;  // never destroyed: guest threads may still run at exit
        for (int host = 0; host < 3; ++host) {
            t->insert(copy_host_stream(host));
        }
        return t;
    }();
    return *table;
}

std::int32_t sceKernelOpen(const char* path, std::int32_t flags, std::uint32_t mode) {
    return sce32(open_file(path, flags, mode));
}

std::int32_t sceKernelClose(std::int32_t fd) {
    return sce32({0, fd_table().remove(fd) ? 0 : EBADF});
}

std::int64_t sceKernelRead(std::int32_t fd, void* buffer, std::uint64_t size) {
    return sce(with_fd(fd, [&](int h) { return from_host(::read(h, buffer, size)); }));
}

std::int64_t sceKernelWrite(std::int32_t fd, const void* buffer, std::uint64_t size) {
    return sce(with_fd(fd, [&](int h) { return from_host(::write(h, buffer, size)); }));
}

std::int64_t sceKernelPread(std::int32_t fd, void* buffer, std::uint64_t size,
                            std::int64_t offset) {
    return sce(with_fd(fd, [&](int h) { return from_host(::pread(h, buffer, size, offset)); }));
}

std::int64_t sceKernelPwrite(std::int32_t fd, const void* buffer, std::uint64_t size,
                             std::int64_t offset) {
    return sce(with_fd(fd, [&](int h) { return from_host(::pwrite(h, buffer, size, offset)); }));
}

std::int64_t sceKernelLseek(std::int32_t fd, std::int64_t offset, std::int32_t whence) {
    const auto host = host_whence(whence);
    return host ? sce(with_fd(fd, [&](int h) { return from_host(::lseek(h, offset, *host)); }))
                : sce_fail(EINVAL);
}

std::int32_t sceKernelFstat(std::int32_t fd, GuestStat* out) {
    return sce32(fstat_file(fd, out));
}

std::int32_t sceKernelStat(const char* path, GuestStat* out) {
    return sce32(stat_path(path, out));
}

std::int32_t sceKernelFsync(std::int32_t fd) {
    return sce32(with_fd(fd, [](int h) { return from_host(::fsync(h)); }));
}

int guest_fstat(std::int32_t fd, GuestStat* out) {
    return posix(fstat_file(fd, out));
}

int posix_stat(const char* path, GuestStat* out) {
    return posix(stat_path(path, out));
}

}  // namespace bb::hle::kernel

namespace bk = bb::hle::kernel;

BB_HLE_FUNCTION("libkernel", "1G3lF1Gg1k8", bk::sceKernelOpen);
BB_HLE_FUNCTION("libkernel", "UK2Tl2DWUns", bk::sceKernelClose);
BB_HLE_FUNCTION("libkernel", "Cg4srZ6TKbU", bk::sceKernelRead);
BB_HLE_FUNCTION("libkernel", "4wSze92BhLI", bk::sceKernelWrite);
BB_HLE_FUNCTION("libkernel", "+r3rMFwItV4", bk::sceKernelPread);
BB_HLE_FUNCTION("libkernel", "nKWi-N2HBV4", bk::sceKernelPwrite);
BB_HLE_FUNCTION("libkernel", "oib76F-12fk", bk::sceKernelLseek);
BB_HLE_FUNCTION("libkernel", "kBwCPsYX-m4", bk::sceKernelFstat);
BB_HLE_FUNCTION("libkernel", "eV9wAD2riIA", bk::sceKernelStat);
BB_HLE_FUNCTION("libkernel", "fTx66l5iWIA", bk::sceKernelFsync);
BB_HLE_FUNCTION("libkernel", "mqQMh1zPPT8", bk::guest_fstat);
BB_HLE_FUNCTION("libScePosix", "E6ao34wPw+U", bk::posix_stat);
