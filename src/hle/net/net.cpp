#include "hle/net/net.hpp"

#include <atomic>
#include <cstring>

#include "common/log.hpp"
#include "hle/hle.hpp"

namespace bb::hle::net {

namespace {

// Hands out 1, 2, 3, … across threads.
class IdCounter {
public:
    std::int32_t next() { return last_.fetch_add(1, std::memory_order_relaxed) + 1; }

private:
    std::atomic<std::int32_t> last_{0};
};

IdCounter pools, ssl_contexts, http_contexts, http_templates, http_epolls;

// The size the only call site (0x1fb6fc6) writes into the block np_manager_sized_request gets.
constexpr std::uint64_t np_block_size = 0x18;

}  // namespace

std::int32_t sceNetInit() {
    return 0;
}

std::int32_t sceNetPoolCreate(const char*, std::int32_t, std::int32_t) {
    return pools.next();
}

std::int32_t sceSslInit(std::size_t) {
    return ssl_contexts.next();
}

std::int32_t sceHttpInit(std::int32_t, std::int32_t, std::size_t) {
    return http_contexts.next();
}

std::int32_t sceHttpCreateTemplate(std::int32_t, const char*, std::int32_t, std::int32_t) {
    return http_templates.next();
}

std::int32_t sceHttpCreateEpoll(std::int32_t, std::uint64_t* handle) {
    *handle = static_cast<std::uint64_t>(http_epolls.next());
    return 0;
}

std::int32_t http_template_set_int(std::int32_t, std::int32_t) {
    return 0;
}

std::int32_t http_template_set_string_pair(std::int32_t, const char*, const char*, std::int32_t) {
    return 0;
}

std::int32_t sceNetCtlGetState(std::int32_t* state) {
    *state = 0;
    return 0;
}

std::int32_t sceNetCtlGetInfo(std::int32_t, void*) {
    return net_ctl_offline;
}

std::int32_t sceNpGetState(std::int32_t, std::int32_t* state) {
    *state = 1;
    return 0;
}

std::int32_t np_manager_sized_request(const void* block) {
    std::uint64_t size = 0;
    std::memcpy(&size, block, sizeof size);
    if (size != np_block_size) {
        common::log(common::LogLevel::warn, "hle.libSceNpManager",
                    "A2CQ3kgSopQ: block size {:#x}, expected {:#x}", size, np_block_size);
    }
    return 0;
}

BB_HLE_FUNCTION("libSceNet", "Nlev7Lg8k3A", sceNetInit);
BB_HLE_FUNCTION("libSceNet", "dgJBaeJnGpo", sceNetPoolCreate);
BB_HLE_FUNCTION("libSceSsl", "hdpVEUDFW3s", sceSslInit);
BB_HLE_FUNCTION("libSceHttp", "A9cVMUtEp4Y", sceHttpInit);
BB_HLE_FUNCTION("libSceHttp", "0gYjPTR-6cY", sceHttpCreateTemplate);
BB_HLE_FUNCTION("libSceHttp", "6381dWF+xsQ", sceHttpCreateEpoll);
BB_HLE_FUNCTION("libSceHttp", "s2-NPIvz+iA", http_template_set_int);         // sceHttpSetNonblock
// sceHttpAddRequestHeader
BB_HLE_FUNCTION("libSceHttp", "EY28T2bkN7k", http_template_set_string_pair);
BB_HLE_FUNCTION("libSceNetCtl", "uBPlr0lbuiI", sceNetCtlGetState);
BB_HLE_FUNCTION("libSceNetCtl", "obuxdTiwkF8", sceNetCtlGetInfo);
BB_HLE_FUNCTION("libSceNpManager", "eQH7nWPcAgc", sceNpGetState);
// sceNpSetContentRestriction
BB_HLE_FUNCTION("libSceNpManager", "A2CQ3kgSopQ", np_manager_sized_request);

}  // namespace bb::hle::net
