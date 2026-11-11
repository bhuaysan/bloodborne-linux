// shadPS4's mount points, as far as AvPlayer opens files by guest path (/app0/movie/...). Guest
// paths go through our kernel's mount table (src/hle/kernel/paths.cpp), so the game directory
// stays read-only and behind one resolver.
#pragma once

#include <filesystem>
#include <memory>
#include <string_view>

#include "core/file_sys/ifile.h"

namespace Core::FileSys {

class MntPoints {
public:
    // Upstream opens through a file backend. Ours has none: always null, so AvPlayer takes its
    // fallback and opens GetHostPath's file directly.
    std::unique_ptr<IFile> Open(std::string_view guest_path, bool writable = false);

    // The host path of `guest_directory`, resolved for reading. Empty (logged) if the kernel
    // refuses it, so opening it fails.
    std::filesystem::path GetHostPath(std::string_view guest_directory);
};

}  // namespace Core::FileSys
