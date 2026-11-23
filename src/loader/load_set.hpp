// The eboot and the bundled PRXs the runtime loads with it (docs/design/m3-core-os.md, "Loading
// the bundled PRXs").
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "loader/image.hpp"

namespace bb::loader {

// The bundled PRXs of the load set in loading order, after the eboot. Each has a hash in
// prx_sha256. The other bundled PRXs are not loaded.
inline constexpr std::array<std::string_view, 2> load_set_prxs = {"libSceFios2.prx", "libc.prx"};

// A function or object that a module exports.
struct Export {
    std::string library, nid;
    std::uintptr_t address;  // runtime
};

// The exports of `image`: its defined NID#library#module symbols other than STT_TLS ones (base +
// st_value is no address for those), in symbol table order. Throws LoadError for an export
// outside the image's segments.
std::vector<Export> exports(const Image& image);

// Reads sce_module/`name` from `game_dir`, checks it against prx_sha256, rebuilds its ELF and
// maps it at `base` with map_elf. Throws LoadError for a name without a pinned hash or another
// build, std::system_error when the file can't be read.
Image map_prx(const std::filesystem::path& game_dir, std::string_view name, std::uintptr_t base);

struct LoadedModule {
    std::string file;  // eboot.bin or the PRX's file name
    std::uint64_t id;  // TLS module id: the position in the load set, from eboot_module_id
    Image image;
};

// Every module of the load set, in loading order. Owns the images.
struct LoadSet {
    std::vector<LoadedModule> modules;

    // The module with eboot_module_id. Throws std::logic_error if there is none.
    LoadedModule& eboot();
    const LoadedModule& eboot() const;
};

// Maps the eboot at `base` (as map_eboot does), then each of load_set_prxs (map_prx) at the next address
// after the previous image that is aligned to the PRX's largest PT_LOAD p_align. Relocates
// nothing. Throws as map_eboot and map_prx do.
LoadSet map_load_set(const std::filesystem::path& game_dir, std::uintptr_t base = default_base);

// As map_load_set, with the eboot's ELF given rather than read from `game_dir`: for the tests,
// which read it once per process. Checks no pin: `eboot_elf` must come from read_pinned_self with
// eboot_bin_sha256, or from the same checks (docs/source-policy.md).
LoadSet map_load_set(const std::filesystem::path& game_dir, std::span<const std::byte> eboot_elf,
                     std::uintptr_t base = default_base);

}  // namespace bb::loader
