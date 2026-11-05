// Rebuilding the ELF inside a PS4 SELF container (docs/formats/self.md).
#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace bb::loader {

// SHA-256 of eboot.bin of CUSA03173 patch 1.09, as listed in docs/clean-room.md.
inline constexpr std::string_view eboot_bin_sha256 =
    "d65f0b4f01d59166aed16f8604196d8b7dd805abbf0758b356e8f1354c9429f9";

// SHA-256 of the bundled PRXs in sce_module/ that the runtime loads, as listed in
// docs/clean-room.md: (file name, digest).
inline constexpr std::array<std::pair<std::string_view, std::string_view>, 2> prx_sha256 = {{
    {"libSceFios2.prx", "3b8eb4d5a5c5a7b381d8d0610077e6cd404c29695a686274802f6b2737f6bf22"},
    {"libc.prx", "4378b47f46f1d856824a6f971db1a0c3f41833b77f49d2fba9538f667a139166"},
}};

// The ELF embedded in an unencrypted, uncompressed SELF, byte for byte as tools/self2elf
// rebuilds it. Checks the result against the SHA-256 digest stored in the SELF. Throws LoadError
// on anything unexpected.
std::vector<std::byte> self_to_elf(std::span<const std::byte> self);

// Reads the SELF `file`, checks that its SHA-256 is `sha256` and returns its ELF (self_to_elf).
// Throws std::system_error when the file can't be read, LoadError when the digest differs.
std::vector<std::byte> read_pinned_self(const std::filesystem::path& file,
                                        std::string_view sha256);

}  // namespace bb::loader
