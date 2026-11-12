// Registration of the imported system libraries in our HLE registry (core/libraries/libs.h).

#include "core/libraries/libs.h"

#include <format>

#include <SDL3/SDL.h>

#include "common/log.hpp"
#include "common/logging/log.h"
#include "core/libraries/ajm/ajm.h"
#include "core/libraries/audio/audioout.h"
#include "core/libraries/avplayer/avplayer.h"
#include "core/libraries/ime/ime_dialog.h"
#include "core/libraries/pad/pad.h"
#include "core/libraries/system/commondialog.h"
#include "hle/hle.hpp"

namespace {
std::size_t linked_functions = 0;
}  // namespace

void LinkSymbolImpl(Core::Loader::SymbolsResolver*, const char* nid, const char* lib, u16,
                    const char*, u64 symbol, Core::Loader::SymbolType sym_type) {
    if (sym_type != Core::Loader::SymbolType::Function) {
        bb::common::log_fatal(
            "shadps4", std::format("LIB_OBJ {} {}: data imports are not supported", lib, nid));
    }
    bb::hle::register_function(lib, nid, reinterpret_cast<void*>(symbol));
    ++linked_functions;
}

namespace Libraries {

void InitHLELibs(Core::Loader::SymbolsResolver* sym, Core::MemoryManager& memory) {
    // Upstream's window starts these before the libraries register: Pad looks for gamepads in
    // its RegisterLib, AudioOut opens devices on sceAudioOutOpen. A failure leaves the game
    // without sound or pads, not without a start. SDL would turn SIGINT and SIGTERM into a quit
    // event nobody reads (the window sets the same hint, but a headless run has none), so the
    // limit of `timeout` would not end the run.
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    for (const auto& [flag, name] : {std::pair{SDL_INIT_AUDIO, "audio"},
                                    std::pair{SDL_INIT_GAMEPAD, "gamepad"}}) {
        if (!SDL_InitSubSystem(flag)) {
            bb::common::log(bb::common::LogLevel::error, "shadps4", "SDL {} subsystem: {}", name,
                            SDL_GetError());
        }
    }
    // The libraries log what they open and create at info level, as upstream's default shows
    // (the translator's classes stay at warnings, host.cpp).
    using Common::Log::Class;
    for (const Class c : {Class::Lib_Pad, Class::Lib_AudioOut, Class::Lib_Ajm,
                          Class::Lib_AvPlayer, Class::Lib_ImeDialog, Class::Lib_CommonDlg,
                          Class::Input}) {
        Common::Log::g_class_levels[static_cast<std::size_t>(c)] = Common::Log::Level::Info;
    }

    Pad::RegisterLib(sym);
    AudioOut::RegisterLib(sym);
    Ajm::RegisterLib(sym);
    AvPlayer::RegisterLib(sym, memory);
    ImeDialog::RegisterLib(sym, memory);
    CommonDialog::RegisterLib(sym);
}

std::size_t LinkedFunctionCount() {
    return linked_functions;
}

}  // namespace Libraries
