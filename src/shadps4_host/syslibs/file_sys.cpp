// AvPlayer's guest paths through our kernel's mount table (core/file_sys/fs.h).

#include <string>

#include "common/log.hpp"
#include "core/file_sys/fs.h"
#include "hle/kernel/paths.hpp"

namespace Core::FileSys {

std::unique_ptr<IFile> MntPoints::Open(std::string_view, bool) {
    return nullptr;
}

std::filesystem::path MntPoints::GetHostPath(std::string_view guest_directory) {
    const bb::hle::kernel::Resolved r =
        bb::hle::kernel::resolve_path(guest_directory, bb::hle::kernel::Access::read);
    if (!r) {
        bb::common::log(bb::common::LogLevel::error, "shadps4",
                        "guest path {} not resolved: errno {}", std::string(guest_directory),
                        r.error);
        return {};
    }
    return r.host;
}

}  // namespace Core::FileSys
