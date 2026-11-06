#include "hle/prx_init.hpp"

#include <atomic>
#include <format>
#include <stdexcept>
#include <utility>
#include <vector>

#include "common/log.hpp"
#include "hle/kernel/process.hpp"
#include "hle/kernel/tls.hpp"
#include "loader/relocate.hpp"

namespace bb::hle {
namespace {

constexpr std::string_view channel = "loader";

}  // namespace

void run_prx_initialiser(std::string_view file, PrxInit* init) {
    common::log(common::LogLevel::info, channel, "{}: running its initialiser", file);
    const std::int32_t result = init(0, nullptr, nullptr);
    if (result != 0) {
        common::log_fatal(channel, std::format("{}: initialiser failed with {:#x}", file,
                                               static_cast<std::uint32_t>(result)));
    }
    common::log(common::LogLevel::info, channel, "{}: initialised", file);
}

void run_prx_init(const loader::LoadSet& set) {
    static std::atomic<bool> ran{false};
    const loader::LoadedModule* eboot = nullptr;
    std::vector<std::pair<const loader::LoadedModule*, PrxInit*>> prxs;
    for (const loader::LoadedModule& m : set.modules) {
        if (m.id == loader::eboot_module_id) {
            eboot = &m;
        } else if (!m.image.dyn.init) {
            throw std::invalid_argument(std::format("{}: no DT_INIT", m.file));
        } else {
            prxs.emplace_back(&m, m.image.fn<PrxInit>(*m.image.dyn.init));
        }
    }
    if (eboot == nullptr || !eboot->image.proc_param) {
        throw std::invalid_argument("the load set's eboot has no PT_SCE_PROCPARAM");
    }
    if (ran.exchange(true)) {
        throw std::logic_error("the PRX initialisers already ran");
    }
    kernel::check_eboot_tls();
    kernel::set_process_parameters(eboot->image.runtime(*eboot->image.proc_param));
    for (const auto& [m, init] : prxs) {
        run_prx_initialiser(m->file, init);
    }
}

}  // namespace bb::hle
