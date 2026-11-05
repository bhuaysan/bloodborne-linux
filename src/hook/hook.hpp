// Function-entry patches that send a game function to its C++ replacement
// (docs/design/runtime.md, "Hooks").
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

#include "loader/image.hpp"

namespace bb::hook {

// A game function and the C++ function that replaces it.
struct Replacement {
    std::uint64_t link_addr;
    void* impl;
    const char* name;  // of the C++ function, for messages
};

// The replacements registered with BB_REPLACE, in registration order. Construct on first use,
// so registrations in other files may run in any order. Registration happens during static
// initialization; a later registration invalidates the span.
std::span<const Replacement> replacements();

// Adds `r` to replacements(). Called by BB_REPLACE; checks nothing, apply does.
void register_replacement(const Replacement& r);

// A static object of this type registers its replacement before main runs.
struct Registrar {
    explicit Registrar(const Replacement& r) { register_replacement(r); }
};

// The patch at a function entry: jmp [rip+0] (FF 25 00000000), then the absolute address.
inline constexpr std::size_t patch_size = 14;

// A refused hook. The message names the replacement, the link address and the reason.
class HookError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Patches the entry of each replaced function in `image` to jump to its replacement, and keeps
// the original bytes for remove. Checks every entry first and throws HookError, patching
// nothing, when an address is not the start of an FDE (Image::function_size), the function is
// shorter than patch_size, its patch overlaps the patch of another entry or of a live hook, or
// impl is null. On a finalized image the affected pages are made writable for the write and
// get their segment's protection back afterwards; throws std::system_error when mprotect fails.
//
// The original bytes live in a registry in bbhook keyed by (image base, link address), so an
// Image doesn't need to know about hooks. An entry whose patch is no longer in memory, because
// its image was unmapped and another one mapped at the same base, counts as not hooked.
// Not thread-safe: hooks are applied and removed while no other thread runs game code.
void apply(loader::Image& image, std::span<const Replacement> replaced);

// Restores the original bytes of each entry and forgets them. Checks every entry first and
// throws HookError, restoring nothing, when one is not hooked in `image`. Protections as for
// apply.
void remove(loader::Image& image, std::span<const Replacement> replaced);

}  // namespace bb::hook

#define BB_HOOK_CONCAT_(a, b) a##b
#define BB_HOOK_CONCAT(a, b) BB_HOOK_CONCAT_(a, b)

// Registers `function` as the replacement of the game function at `link_addr`, at namespace
// scope in the file that defines it: BB_REPLACE(0x00123450, my_function);
// A target that uses it must be an OBJECT library (docs/design/runtime.md, "Static
// registration"). The trailing static_assert takes the semicolon.
#define BB_REPLACE(link_addr, function)                                                 \
    namespace {                                                                         \
    const ::bb::hook::Registrar BB_HOOK_CONCAT(bb_replace_, __COUNTER__){                \
        ::bb::hook::Replacement{(link_addr), reinterpret_cast<void*>(&(function)), #function}}; \
    }                                                                                   \
    static_assert(true)
