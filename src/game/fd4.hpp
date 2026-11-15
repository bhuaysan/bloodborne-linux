// Calls into the game's FD4 engine that several replacements share.
#pragma once

#include <cstdint>

#include "game/image.hpp"

namespace bb::game {

// The engine's report of a singleton that is used before it exists (0x020b55b0), with the
// arguments FD4Singleton.h passes: the header's path, line 0xb1, the message format and
// `name_link`, the link address of the singleton's class name. The game goes on after it.
inline void singleton_missing(std::uint64_t name_link) {
    using Report = void(const char* file, int line, const char* format, const char* name, ...);
    const auto text = [](std::uint64_t link) {
        return reinterpret_cast<const char*>(image().runtime(link));
    };
    fn<Report>(0x020b55b0)(text(0x0493b369), 0xb1, text(0x0493b3bd), text(name_link));
}

}  // namespace bb::game
