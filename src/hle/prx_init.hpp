// Running the bundled PRXs' initialisers (docs/design/m3-core-os.md, "Loading the bundled PRXs",
// Init).
#pragma once

#include <cstdint>
#include <string_view>

#include "loader/load_set.hpp"

namespace bb::hle {

// A PRX's DT_INIT, at link address 0 in both PRXs: it runs the preinit array and the constructor
// list, then tail-calls `next(arg, argp)` if `next` is non-null, else module_start. Returns the
// result of that call, or 0 when the module has no module_start.
using PrxNext = std::int32_t(std::uint64_t arg, const void* argp);
using PrxInit = std::int32_t(std::uint64_t arg, const void* argp, PrxNext* next);

// Calls `init(0, nullptr, nullptr)` for the PRX `file` and logs its result. A non-zero result
// logs a fatal line naming the file and the result and aborts the start.
void run_prx_initialiser(std::string_view file, PrxInit* init);

// Runs on the game's main thread before the eboot's entry, once `set` is linked (link()) and its
// protections are final: checks the calling thread's eboot TLS block (kernel::check_eboot_tls),
// hands the eboot's PT_SCE_PROCPARAM block to libkernel (kernel::set_process_parameters), then
// runs run_prx_initialiser for every PRX of `set` in loading order. The eboot's own DT_INIT is
// left to its entry. libc.prx's preinit fills its allocator dispatch from the eboot's tables, so
// from here on libc's malloc family runs the game's allocator. Throws std::invalid_argument when
// the eboot (the module with eboot_module_id) has no PT_SCE_PROCPARAM or a PRX has no DT_INIT,
// std::logic_error when called a second time; both before any game code runs.
void run_prx_init(const loader::LoadSet& set);

}  // namespace bb::hle
