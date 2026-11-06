#include "hle/kernel/dir.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>

#include "common/log.hpp"
#include "hle/hle.hpp"
#include "hle/kernel/abi.hpp"
#include "hle/kernel/file.hpp"
#include "hle/kernel/paths.hpp"

namespace bb::hle::kernel {
namespace {

constexpr std::string_view channel = "hle.libkernel";

// One record of the host's getdents64. glibc declares no struct for the raw system call.
struct HostDirent {
    std::uint64_t ino;
    std::int64_t off;  // the read position after this record
    std::uint16_t reclen;
    std::uint8_t type;
    char name[1];
};

// A host errno, or 0 on success.
int host_result(long result) {
    return result < 0 ? errno : 0;
}

void log_call(std::string_view call, const char* path, int error) {
    common::log(common::LogLevel::debug, channel, "{} '{}': host errno {}", call,
                path != nullptr ? path : "(null)", error);
}

// Runs `f` on the host path of `path`, resolved for writing.
template <class F>
int on_path(std::string_view call, const char* path, F f) {
    Resolved r;
    int error = resolve_guest_path(path, Access::write, r);
    if (error == 0) {
        error = f(r);
    }
    log_call(call, path, error);
    return error;
}

int make_dir(const char* path, std::uint32_t mode) {
    return on_path("mkdir", path, [&](const Resolved& r) {
        return host_result(::mkdir(r.host.c_str(), static_cast<mode_t>(mode & 07777)));
    });
}

// Removing a mount point would remove the host directory behind it, so it is busy, as a mount
// point is on the guest.
int remove_dir(const char* path) {
    return on_path("rmdir", path, [](const Resolved& r) {
        return r.mount_root ? EBUSY : host_result(::rmdir(r.host.c_str()));
    });
}

int rename_path(const char* from, const char* to) {
    Resolved source;
    Resolved target;
    int error = resolve_guest_path(from, Access::write, source);
    if (error == 0) {
        error = resolve_guest_path(to, Access::write, target);
    }
    if (error == 0) {
        // The mounts may share a host file system, where the host rename would succeed.
        if (source.mount != target.mount) {
            error = EXDEV;
        } else if (source.mount_root || target.mount_root) {
            error = EBUSY;
        } else {
            error = host_result(::rename(source.host.c_str(), target.host.c_str()));
        }
    }
    common::log(common::LogLevel::debug, channel, "rename '{}' to '{}': host errno {}",
                from != nullptr ? from : "(null)", to != nullptr ? to : "(null)", error);
    return error;
}

// The guest's 32-bit file number for a host inode: truncated like st_ino, but never 0, since
// libSceFios2 (0x2d453) skips records with file number 0.
std::uint32_t guest_fileno(std::uint64_t ino) {
    const auto fileno = static_cast<std::uint32_t>(ino);
    return fileno != 0 ? fileno : 0xffffffffu;
}

struct Listing {
    std::int32_t length = 0;
    int error = 0;
};

// Copies host records of `h` into `out` until the next one doesn't fit or the directory ends,
// then leaves the host read position right after the last record taken or skipped.
Listing list_dir(int h, std::span<std::byte> out, std::int64_t* base) {
    const off_t start = ::lseek(h, 0, SEEK_CUR);
    if (start < 0) {
        // A pipe or tty (such as the host streams at guest 0-2) isn't a directory either.
        return {0, errno == ESPIPE ? EINVAL : errno};
    }
    alignas(HostDirent) std::byte host[8192];
    std::size_t used = 0;
    off_t position = start;
    bool full = false;
    int error = 0;
    while (!full) {
        const long n = ::syscall(SYS_getdents64, h, host, sizeof host);
        if (n <= 0) {
            // A descriptor that isn't a directory is EINVAL on the guest, as in FreeBSD.
            error = n == 0 ? 0 : (errno == ENOTDIR ? EINVAL : errno);
            break;
        }
        for (long at = 0; at < n;) {
            HostDirent entry;
            std::memcpy(&entry, host + at, offsetof(HostDirent, name));
            const std::string_view name(reinterpret_cast<const char*>(host + at) +
                                        offsetof(HostDirent, name));
            if (!is_junk_name(name)) {
                const std::size_t length =
                    put_dirent(out.subspan(used), guest_fileno(entry.ino),
                               guest_dirent_type(h, name.data(), entry.type), name);
                if (length == 0) {
                    full = true;
                    break;
                }
                used += length;
            }
            position = entry.off;
            at += entry.reclen;
        }
    }
    // The host read ahead past the records that didn't fit: go back to the first of them.
    if ((full || error != 0) && ::lseek(h, position, SEEK_SET) < 0 && error == 0) {
        error = errno;
    }
    if (used == 0 && (full || error != 0)) {
        return {0, error != 0 ? error : EINVAL};
    }
    if (base != nullptr) {
        *base = start;
    }
    return {static_cast<std::int32_t>(used), 0};
}

}  // namespace

std::uint8_t guest_dirent_type(int dir, const char* name, std::uint8_t host_type) {
    if (host_type != DT_UNKNOWN && host_type != DT_LNK) {
        return host_type;
    }
    struct stat st;
    if (::fstatat(dir, name, &st, 0) != 0) {
        return host_type;
    }
    return static_cast<std::uint8_t>(IFTODT(st.st_mode));
}

std::int32_t sceKernelMkdir(const char* path, std::uint32_t mode) {
    return sce_fail(make_dir(path, mode));
}

std::int32_t sceKernelRmdir(const char* path) {
    return sce_fail(remove_dir(path));
}

std::int32_t sceKernelUnlink(const char* path) {
    return sce_fail(on_path("unlink", path, [](const Resolved& r) {
        if (r.mount_root) {
            return EBUSY;
        }
        const int error = host_result(::unlink(r.host.c_str()));
        // Linux reports a directory as EISDIR, FreeBSD's unlink(2) as EPERM.
        return error == EISDIR ? EPERM : error;
    }));
}

std::int32_t sceKernelRename(const char* from, const char* to) {
    return sce_fail(rename_path(from, to));
}

std::int32_t sceKernelTruncate(const char* path, std::int64_t length) {
    return sce_fail(on_path("truncate", path, [&](const Resolved& r) {
        return host_result(::truncate(r.host.c_str(), length));
    }));
}

std::int32_t sceKernelFtruncate(std::int32_t fd, std::int64_t length) {
    const std::shared_ptr<OpenFile> file = fd_table().get(fd);
    return sce_fail(file ? host_result(::ftruncate(file->host_fd, length)) : EBADF);
}

std::int32_t sceKernelGetdirentries(std::int32_t fd, void* buffer, std::int32_t size,
                                    std::int64_t* base) {
    const std::shared_ptr<OpenFile> file = fd_table().get(fd);
    if (!file) {
        return sce_fail(EBADF);
    }
    if (size < 0) {
        return sce_fail(EINVAL);
    }
    if (buffer == nullptr) {
        return sce_fail(EFAULT);
    }
    const Listing l = list_dir(
        file->host_fd, {static_cast<std::byte*>(buffer), static_cast<std::size_t>(size)}, base);
    return l.error != 0 ? sce_fail(l.error) : l.length;
}

int posix_mkdir(const char* path, std::uint32_t mode) {
    const int error = make_dir(path, mode);
    return error != 0 ? posix_fail(error) : 0;
}

int posix_rmdir(const char* path) {
    const int error = remove_dir(path);
    return error != 0 ? posix_fail(error) : 0;
}

int posix_rename(const char* from, const char* to) {
    const int error = rename_path(from, to);
    return error != 0 ? posix_fail(error) : 0;
}

}  // namespace bb::hle::kernel

namespace bk = bb::hle::kernel;

BB_HLE_FUNCTION("libkernel", "1-LFLmRFxxM", bk::sceKernelMkdir);
BB_HLE_FUNCTION("libkernel", "naInUjYt3so", bk::sceKernelRmdir);
BB_HLE_FUNCTION("libkernel", "AUXVxWeJU-A", bk::sceKernelUnlink);
BB_HLE_FUNCTION("libkernel", "52NcYU9+lEo", bk::sceKernelRename);
BB_HLE_FUNCTION("libkernel", "WlyEA-sLDf0", bk::sceKernelTruncate);
BB_HLE_FUNCTION("libkernel", "VW3TVZiM4-E", bk::sceKernelFtruncate);
BB_HLE_FUNCTION("libkernel", "taRWhTJFTgE", bk::sceKernelGetdirentries);
BB_HLE_FUNCTION("libScePosix", "JGMio+21L4c", bk::posix_mkdir);
BB_HLE_FUNCTION("libScePosix", "c7ZnT7V1B98", bk::posix_rmdir);
BB_HLE_FUNCTION("libScePosix", "NN01qLRhiqU", bk::posix_rename);
