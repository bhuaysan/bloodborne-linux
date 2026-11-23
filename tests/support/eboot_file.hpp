// The game's eboot.bin for the tests against it, read once per process (#71).
#pragma once

#include <cstddef>
#include <filesystem>
#include <vector>

namespace bb::test {

// eboot.bin and the ELF that loader::self_to_elf rebuilds from it.
struct EbootFile {
    std::vector<std::byte> self, elf;
};

// Reads eboot.bin from `game` (from BB_REQUIRE_GAME) on the first call, checks that it is the
// pinned build (loader::eboot_bin_sha256) and rebuilds its ELF. Later calls return the same
// object. Throws loader::LoadError when the file is another build, std::system_error when it
// can't be read; the next call tries again.
const EbootFile& eboot_file(const std::filesystem::path& game);

}  // namespace bb::test
