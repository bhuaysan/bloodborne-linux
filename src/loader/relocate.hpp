// Applying the relocations of a mapped image (docs/design/runtime.md, "Relocations").
#pragma once

#include <cstdint>
#include <functional>

#include "loader/dynamic.hpp"
#include "loader/image.hpp"

namespace bb::loader {

// Returns the runtime address of an import. Must not return 0.
using Resolver = std::function<std::uintptr_t(const Import&)>;

// Applies every relocation of DT_SCE_RELA, then of DT_SCE_JMPREL:
//   R_X86_64_RELATIVE                       base + addend
//   R_X86_64_64                             S + addend
//   R_X86_64_GLOB_DAT, R_X86_64_JUMP_SLOT   S
// S is base + st_value for a symbol defined in the image, `resolve(import)` for an import and 0
// for the weak plain module_start and module_stop. Each 8-byte target must lie inside a PT_LOAD,
// and inside a writable one unless the image has DT_TEXTREL. Throws LoadError naming the type and
// the target for any other type or symbol, a bad target or an import resolved to 0; the image is
// then partly relocated and unusable. Throws std::logic_error after finalize_protection().
void relocate(Image& image, const Resolver& resolve);

}  // namespace bb::loader
