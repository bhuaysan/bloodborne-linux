// Function-entry patches that send a game function to its C++ replacement
// (docs/design/runtime.md, "Hooks").
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

#include "common/macros.hpp"
#include "loader/image.hpp"

namespace bb::hook {

// A game function and the C++ function that replaces it.
struct Replacement {
    std::uint64_t link_addr;
    void* impl;
    const char* name;  // of the C++ function, for messages
    // For a replacement that calls the original (BB_WRAP): the function's first bytes, whole
    // instructions of at least patch_size bytes that don't depend on where they run (no
    // rip-relative operands, no relative jumps or calls). apply copies them into a trampoline
    // that original() returns. Empty for a plain replacement.
    std::span<const std::uint8_t> prologue{};
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
//
// An entry with a prologue is also refused when the prologue is shorter than patch_size, longer
// than the function or than max_prologue, or differs from the bytes in memory.
void apply(loader::Image& image, std::span<const Replacement> replaced);

// The longest prologue a trampoline takes.
inline constexpr std::size_t max_prologue = 32;

// The original of the function at `link` in `image`, for a replacement that wraps it: while it
// is hooked with a prologue, a trampoline that runs the prologue and jumps to the instruction
// behind it; while it isn't hooked (as in the harness), the function itself. Throws HookError
// when it is hooked without a prologue. The trampoline lives until the hook is removed, so
// callers ask again instead of keeping the address.
void* original(const loader::Image& image, std::uint64_t link);

// Restores the original bytes of each entry and forgets them. Checks every entry first and
// throws HookError, restoring nothing, when one is not hooked in `image`. Protections as for
// apply.
void remove(loader::Image& image, std::span<const Replacement> replaced);

}  // namespace bb::hook

// Registers `function` as the replacement of the game function at `link_addr`, at namespace
// scope in the file that defines it: BB_REPLACE(0x00123450, my_function);
// A target that uses it must be an OBJECT library (docs/design/runtime.md, "Static
// registration"). The trailing static_assert takes the semicolon.
#define BB_REPLACE(link_addr, function)                                                 \
    namespace {                                                                         \
    const ::bb::hook::Registrar BB_CONCAT(bb_replace_, __COUNTER__){                     \
        ::bb::hook::Replacement{(link_addr), reinterpret_cast<void*>(&(function)), #function}}; \
    }                                                                                   \
    static_assert(true)

// As BB_REPLACE, for a replacement that calls the original through bb::hook::original.
// `prologue` names an array of the function's first bytes with static storage duration (see
// Replacement::prologue).
#define BB_WRAP(link_addr, function, prologue)                                          \
    namespace {                                                                         \
    const ::bb::hook::Registrar BB_CONCAT(bb_replace_, __COUNTER__){                     \
        ::bb::hook::Replacement{(link_addr), reinterpret_cast<void*>(&(function)), #function, \
                                (prologue)}};                                           \
    }                                                                                   \
    static_assert(true)
