// How the imported system libraries register their functions (docs/design/os-libraries.md).
// Each library's RegisterLib(sym) lists LIB_FUNCTION(nid, library, version, module, function);
// here that adds the function to our HLE registry (src/hle/hle.hpp), keyed by library and NID
// as the game's imports are. A NID that src/hle registers as well throws std::logic_error, so
// startup fails: every NID has one owner.
#pragma once

#include <cstddef>

#include "common/types.h"
#include "core/tls.h"

namespace Core::Loader {

// Upstream's symbol table. Ours is the HLE registry, so this is only the tag RegisterLib passes on.
class SymbolsResolver {};

enum class SymbolType { Function, Object };

}  // namespace Core::Loader

// Registers `symbol` for the import (lib, nid). The version and module are not part of our key.
// Objects (LIB_OBJ) end the process: none of the imported libraries registers one.
void LinkSymbolImpl(Core::Loader::SymbolsResolver* sym, const char* nid, const char* lib,
                    u16 libversion, const char* mod, u64 symbol, Core::Loader::SymbolType sym_type);

// Guest code calls the functions directly: the SysV ABI is the host's (PS4_SYSV_ABI).
#define LIB_FUNCTION(nid, lib, libversion, mod, function)                                     \
    LinkSymbolImpl(sym, nid, lib, libversion, mod, reinterpret_cast<u64>(&(function)),        \
                   Core::Loader::SymbolType::Function)

#define LIB_OBJ(nid, lib, libversion, mod, obj)                                               \
    LinkSymbolImpl(sym, nid, lib, libversion, mod, reinterpret_cast<u64>(obj),                \
                   Core::Loader::SymbolType::Object)

namespace Core {
class MemoryManager;
}

namespace Libraries {

// Registers Pad, AudioOut, Ajm, AvPlayer, ImeDialog and CommonDialog, after starting the SDL
// subsystems they use (audio, gamepads). Call once, before the game's imports are linked
// (bb::hle::link); throws std::logic_error for a NID that is registered already.
void InitHLELibs(Core::Loader::SymbolsResolver* sym, Core::MemoryManager& memory);

// The number of (library, NID) pairs LinkSymbolImpl has registered so far. The test checks it
// against the libraries' LIB_FUNCTION lines, so a RegisterLib that drops functions shows.
std::size_t LinkedFunctionCount();

}  // namespace Libraries
