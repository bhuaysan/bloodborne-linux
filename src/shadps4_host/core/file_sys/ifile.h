// The piece of shadPS4's file backend interface that AvPlayer's handle streamer uses. Our
// MntPoints (core/file_sys/fs.h) never hands out a file, so no backend implements it here.
#pragma once

#include <filesystem>
#include <optional>

#include "common/io_file.h"
#include "common/types.h"

namespace Core::FileSys {

class IFile {
public:
    virtual ~IFile() = default;
    virtual s64 Read(void* dst, u64 size) = 0;
    virtual bool Seek(s64 offset, Common::FS::SeekOrigin origin) = 0;
    virtual u64 Size() const = 0;
    virtual bool IsOpen() const = 0;
    virtual std::optional<std::filesystem::path> GetHostPath() const { return std::nullopt; }
};

}  // namespace Core::FileSys
