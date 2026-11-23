// libSceAppContent: fixed app parameters and the one add-on of the Game of the Year edition.

#include <cstdint>
#include <cstring>
#include <string_view>

#include "hle/hle.hpp"
#include "hle/system/log.hpp"
#include "hle/system/system.hpp"

namespace bb::hle::system {
namespace {

constexpr std::string_view library = "libSceAppContent";

}  // namespace

std::int32_t app_content_initialize(const void* /*init_params*/, void* /*boot_params*/) {
    // The game zeroes both blocks before the call (0x2026167 to 0x20261c6), ignores the result
    // and never reads either block afterwards, so we leave them alone.
    log(library, BB_SYSTEM_CALLER(), "initialize");
    return 0;
}

std::int32_t app_content_get_int(std::int32_t key, std::int32_t* value) {
    if (key < 0 || key >= app_param_keys) {
        log(library, BB_SYSTEM_CALLER(), "integer parameter: unknown key {}", key);
        return app_content_bb_error_unknown_key;
    }
    if (value == nullptr) {
        return app_content_bb_error_invalid_argument;
    }
    *value = app_param_value[key];
    return 0;
}

std::int32_t app_content_get_addcont_info_list(std::uint32_t service_label, AddcontInfo* list,
                                              std::uint32_t list_size, std::uint32_t* count) {
    const std::uint32_t items = service_label == 0 ? 1 : 0;
    if (list == nullptr || list_size == 0) {
        if (count == nullptr) {
            return app_content_bb_error_invalid_argument;
        }
        *count = items;
        return 0;
    }
    const std::uint32_t written = items < list_size ? items : list_size;
    for (std::uint32_t i = 0; i < written; ++i) {
        list[i] = AddcontInfo{};
        std::memcpy(list[i].entitlement_label, old_hunters_label.data(), old_hunters_label.size());
        list[i].status = addcont_status_no_extra_data;
    }
    if (count != nullptr) {
        *count = written;
    }
    return 0;
}

BB_HLE_FUNCTION("libSceAppContent", "R9lA82OraNs", app_content_initialize);
// sceAppContentAppParamGetInt
BB_HLE_FUNCTION("libSceAppContent", "99b82IKXpH4", app_content_get_int);
// sceAppContentGetAddcontInfoList
BB_HLE_FUNCTION("libSceAppContent", "xnd8BJzAxmk", app_content_get_addcont_info_list);

}  // namespace bb::hle::system
