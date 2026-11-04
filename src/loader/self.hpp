// Rebuilding the ELF inside a PS4 SELF container (docs/formats/self.md).
#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace bb::loader {

// SHA-256 of eboot.bin of CUSA03173 patch 1.09, as listed in docs/clean-room.md.
inline constexpr std::string_view eboot_bin_sha256 =
    "d65f0b4f01d59166aed16f8604196d8b7dd805abbf0758b356e8f1354c9429f9";

// The ELF embedded in an unencrypted, uncompressed SELF, byte for byte as tools/self2elf
// rebuilds it. Checks the result against the SHA-256 digest stored in the SELF. Throws LoadError
// on anything unexpected.
std::vector<std::byte> self_to_elf(std::span<const std::byte> self);

}  // namespace bb::loader
