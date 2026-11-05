// libSceSysmodule: the game loads system modules by id. We track the ids and report success.

#include <cstdint>
#include <mutex>
#include <set>
#include <string_view>

#include "hle/hle.hpp"
#include "hle/system/internal.hpp"
#include "hle/system/log.hpp"
#include "hle/system/system.hpp"

namespace bb::hle::system {
namespace {

constexpr std::string_view library = "libSceSysmodule";

struct Loaded {
    std::mutex mutex;
    std::set<std::uint32_t> ids;
};

Loaded& loaded() {
    static Loaded& l = *new Loaded;  // leaked: game threads may call in during exit
    return l;
}

}  // namespace

std::string_view sysmodule_library(std::uint32_t id) {
    // docs/formats/imports.md, "Modules loaded at runtime".
    switch (id) {
    case 0x83: return "libScePlayGo";
    case 0x96: return "libSceImeDialog";
    case 0x9d: return "libSceNpAuth";
    case 0xa0: return "libSceSaveData";
    case 0xa4: return "libSceMsgDialog";
    case 0xa5: return "libSceAvPlayer";
    case 0xa8: return "libSceNpCommerce";
    case 0xa9: return "libSceMouse";
    case 0xad: return "libSceNpTrophy";
    case 0xb4: return "libSceAppContent";
    case 0x1e: return "libSceNpScore";
    case 0x19:
    case 0x1a:
    case 0x1c:
    case 0xb2:
    case 0xb5: return "online subsystem";
    case 0xd7: return "libSceDiscMap";
    default: return "unknown";
    }
}

std::int32_t sysmodule_load_module(std::uint32_t id) {
    // Every load succeeds. 0xb4 must (0x2026149), and libSceFios2 fails its init on any other
    // result for 0xd7 (libSceFios2 0x2d795).
    log(library, BB_SYSTEM_CALLER(), "load module {:#x} ({})", id, sysmodule_library(id));
    Loaded& l = loaded();
    const std::lock_guard lock(l.mutex);
    l.ids.insert(id);
    return 0;
}

std::int32_t sysmodule_unload_module(std::uint32_t id) {
    log(library, BB_SYSTEM_CALLER(), "unload module {:#x} ({})", id, sysmodule_library(id));
    Loaded& l = loaded();
    const std::lock_guard lock(l.mutex);
    return l.ids.erase(id) != 0 ? 0 : sysmodule_error_not_loaded;
}

std::int32_t sysmodule_is_loaded(std::uint32_t id) {
    // Known ids are polled often, so only unknown ones are logged.
    if (sysmodule_library(id) == "unknown") {
        log(library, BB_SYSTEM_CALLER(), "is loaded {:#x} (unknown)", id);
    }
    Loaded& l = loaded();
    const std::lock_guard lock(l.mutex);
    return l.ids.contains(id) ? 0 : sysmodule_error_not_loaded;
}

void reset_sysmodule() {
    Loaded& l = loaded();
    const std::lock_guard lock(l.mutex);
    l.ids.clear();
}

BB_HLE_FUNCTION("libSceSysmodule", "g8cM39EUZ6o", sysmodule_load_module);
BB_HLE_FUNCTION("libSceSysmodule", "eR2bZFAAU0Q", sysmodule_unload_module);
BB_HLE_FUNCTION("libSceSysmodule", "fMP5NHUOaMk", sysmodule_is_loaded);

}  // namespace bb::hle::system
