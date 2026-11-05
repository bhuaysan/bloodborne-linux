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
