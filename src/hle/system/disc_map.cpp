// libSceDiscMap, imported by libSceFios2 only: no path is remapped.

#include <atomic>
#include <cstdint>
#include <string_view>

#include "hle/hle.hpp"
#include "hle/system/log.hpp"
#include "hle/system/system.hpp"

namespace bb::hle::system {
namespace {

constexpr std::string_view library = "libSceDiscMap";

// libSceFios2 calls both per file operation, so only the first calls are logged.
std::atomic<std::uint64_t> calls = 0;

void log_call(const void* caller, std::string_view what, const char* path) {
    if (should_log(++calls)) {
        log(library, caller, "{} \"{}\": not remapped", what, path != nullptr ? path : "(null)");
    }
}

}  // namespace

std::int32_t disc_map_is_remapped(const char* path, std::uint64_t /*arg1*/,
                                  std::uint64_t /*arg2*/, std::int32_t* out) {
    log_call(BB_SYSTEM_CALLER(), "lbQKqsERhtE", path);
    if (out == nullptr) {
        return disc_map_bb_error_not_mapped;
    }
    *out = 0;
    return 0;
}

std::int32_t disc_map_map_range(const char* path, std::uint64_t /*offset*/,
                                std::uint64_t /*size*/, std::uint64_t* /*out1*/,
                                std::uint64_t* /*out2*/, std::uint64_t* /*out3*/) {
    log_call(BB_SYSTEM_CALLER(), "ioKMruft1ek", path);
    return disc_map_bb_error_not_mapped;
}

BB_HLE_FUNCTION("libSceDiscMap", "lbQKqsERhtE", disc_map_is_remapped);  // sceDiscMapIsRequestOnHDD
BB_HLE_FUNCTION("libSceDiscMap", "ioKMruft1ek", disc_map_map_range);  // name unknown

}  // namespace bb::hle::system
