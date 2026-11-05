#include "common/file.hpp"

#include <sys/stat.h>

#include <cerrno>
#include <cstdio>
#include <memory>
#include <system_error>

namespace bb::common {

std::vector<std::byte> read_file(const std::filesystem::path& path) {
    auto fail = [&](int err) {
        return std::system_error(err, std::generic_category(), "cannot read " + path.string());
    };
    using File = std::unique_ptr<std::FILE, int (*)(std::FILE*)>;
    File file(std::fopen(path.c_str(), "rb"), std::fclose);
    if (!file) {
        throw fail(errno);
    }
    // The size at open time. Files in /proc report 0, and a file may change while it is read, so
    // what was read counts: the result shrinks to it, or grows until EOF.
    struct stat st {};
    std::size_t size = 0;
    if (fstat(fileno(file.get()), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
        size = static_cast<std::size_t>(st.st_size);
    }
    std::vector<std::byte> data(size);
    std::size_t got = size == 0 ? 0 : std::fread(data.data(), 1, size, file.get());
    if (got < size) {
        data.resize(got);
    } else {
        std::byte chunk[1 << 12];
        while (std::size_t n = std::fread(chunk, 1, sizeof chunk, file.get())) {
            data.insert(data.end(), chunk, chunk + n);
        }
    }
    if (std::ferror(file.get())) {
        throw fail(errno ? errno : EIO);
    }
    return data;
}

}  // namespace bb::common
