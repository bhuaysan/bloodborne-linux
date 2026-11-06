#include "hle/kernel/dir.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <memory>
#include <string_view>

#include "common/log.hpp"
#include "hle/hle.hpp"
#include "hle/kernel/abi.hpp"
#include "hle/kernel/file.hpp"
#include "hle/kernel/paths.hpp"

namespace bb::hle::kernel {
namespace {

constexpr std::string_view channel = "hle.libkernel";

// A host errno, or 0 on success.
int host_result(long result) {
    return result < 0 ? errno : 0;
}

// The host path of `path` in `out`, or the host errno.
int resolve(const char* path, Access access, Resolved& out) {
    if (path == nullptr) {
        return EFAULT;
    }
    out = resolve_path(path, access);
    return out ? 0 : host_errno(out.error).value_or(EIO);
}

void log_call(std::string_view call, const char* path, int error) {
    common::log(common::LogLevel::debug, channel, "{} '{}': host errno {}", call,
                path != nullptr ? path : "(null)", error);
}

// Runs `f` on the host path of `path`, resolved for writing.
template <class F>
int on_path(std::string_view call, const char* path, F f) {
    Resolved r;
    int error = resolve(path, Access::write, r);
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
    int error = resolve(from, Access::write, source);
    if (error == 0) {
        error = resolve(to, Access::write, target);
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

}  // namespace

std::int32_t sceKernelMkdir(const char* path, std::uint32_t mode) {
    return sce_fail(make_dir(path, mode));
}

std::int32_t sceKernelRmdir(const char* path) {
    return sce_fail(remove_dir(path));
}

std::int32_t sceKernelUnlink(const char* path) {
    return sce_fail(on_path("unlink", path, [](const Resolved& r) {
        return r.mount_root ? EBUSY : host_result(::unlink(r.host.c_str()));
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
BB_HLE_FUNCTION("libScePosix", "JGMio+21L4c", bk::posix_mkdir);
BB_HLE_FUNCTION("libScePosix", "c7ZnT7V1B98", bk::posix_rmdir);
BB_HLE_FUNCTION("libScePosix", "NN01qLRhiqU", bk::posix_rename);
