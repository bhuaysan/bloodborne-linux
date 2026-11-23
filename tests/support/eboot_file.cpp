#include "support/eboot_file.hpp"

#include <memory>
#include <string>
#include <utility>

#include "common/file.hpp"
#include "common/sha256.hpp"
#include "loader/bytes.hpp"
#include "loader/self.hpp"

namespace bb::test {

const EbootFile& eboot_file(const std::filesystem::path& game) {
    static std::unique_ptr<const EbootFile> eboot;
    if (!eboot) {
        EbootFile read;
        read.self = common::read_file(game / "eboot.bin");
        const std::string digest = common::to_hex(common::sha256(read.self));
        if (digest != loader::eboot_bin_sha256) {
            throw loader::LoadError("eboot.bin has SHA-256 " + digest + ", not the pinned build");
        }
        read.elf = loader::self_to_elf(read.self);
        eboot = std::make_unique<const EbootFile>(std::move(read));
    }
    return *eboot;
}

}  // namespace bb::test
