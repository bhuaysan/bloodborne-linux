// Reading whole files.
#pragma once

#include <cstddef>
#include <filesystem>
#include <vector>

namespace bb::common {

// The whole file. Throws std::system_error naming the path when it can't be read.
std::vector<std::byte> read_file(const std::filesystem::path& path);

}  // namespace bb::common
