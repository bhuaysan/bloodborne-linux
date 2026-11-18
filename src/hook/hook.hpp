// Patches that send a game function, or a few instructions inside one, to C++ or a stub
// (docs/design/runtime.md, "Hooks").
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

#include "common/macros.hpp"
#include "loader/image.hpp"

namespace bb::hook {

// A place inside a patched function that its stub jumps to, besides the bytes behind the patch
// (Replacement::targets): apply writes the runtime address of `link_addr` to `*slot`.
struct JumpTarget {
    std::uint64_t link_addr;
    std::uint64_t* slot;
};

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
    // For a patch inside a function (BB_PATCH): the link address of that function, an FDE start.
    // link_addr is then an instruction after its entry and `prologue` the bytes from there that
    // impl takes over: whole instructions, at least patch_size and at most max_patched bytes, all
    // inside the function. impl is a stub written for that place, entered by a jump with the
    // registers and stack of the function, that does their work and jumps back into the function
    // itself. No trampoline. 0 for a patch at an entry.
    std::uint64_t function = 0;
    // For a patch inside a function: the global the stub jumps back through
    // (jmp [rip + continuation]). apply writes the runtime address right behind the patched
    // bytes to it before it writes the patch; a patch without one is refused.
    std::uint64_t* continuation = nullptr;
    // For a patch inside a function: the other places its stub jumps to, each inside the
    // function. apply writes their runtime addresses as it writes the continuation.
    std::span<const JumpTarget> targets{};
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
// An entry at a function's entry is also refused when it has a continuation or jump targets
// (they belong to a patch inside a function). One with a prologue is refused when the prologue
// is shorter than patch_size, longer than the function or than max_prologue, or differs from the
// bytes in memory, and any entry whose patch overlaps the prologue of a live hook (the trampoline
// jumps back behind it).
//
// A patch inside a function (`function` set) is refused instead of the entry checks when
// `function` is not an FDE start, link_addr is not after it, the patched bytes are fewer than
// patch_size, more than max_patched or don't all lie inside the function, or differ from the bytes
// in memory, or when it has no continuation, or a jump target has no slot or lies outside the
// function. Its span (the patched bytes) must not overlap another entry's: two patches of the
// list, or a live hook's patch or prologue. Once every entry passes, apply writes the
// continuations and jump targets, then the patches; remove leaves them as they are.
void apply(loader::Image& image, std::span<const Replacement> replaced);

// The longest prologue a trampoline takes.
inline constexpr std::size_t max_prologue = 32;

// The most bytes a patch inside a function takes over.
inline constexpr std::size_t max_patched = 64;

// The original of the function at `link` in `image`, for a replacement that wraps it: while it
// is hooked with a prologue, a trampoline that runs the prologue and jumps to the instruction
// behind it; while it isn't hooked (as in the harness), the function itself. Throws HookError
// when it is hooked without a prologue or patched inside a function. The trampoline lives until
// the hook is removed, so callers ask again instead of keeping the address. Safe from any thread
// while no apply or remove runs.
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

// Registers `stub` as a patch inside the game function at `function_addr`, starting at the
// instruction at `link_addr`; `patched` names an array of the bytes it takes over, with static
// storage duration (see Replacement::function). The stub is code written for that place that
// jumps back through the std::uint64_t global `continuation`, which apply fills.
#define BB_PATCH(function_addr, link_addr, stub, patched, continuation)                 \
    BB_PATCH_TARGETS(function_addr, link_addr, stub, patched, continuation,           \
                     ::std::span<const ::bb::hook::JumpTarget>{})

// As BB_PATCH, for a stub that also jumps to other places of the function: `targets` names an
// array of bb::hook::JumpTarget with static storage duration, whose slots apply fills.
#define BB_PATCH_TARGETS(function_addr, link_addr, stub, patched, continuation, targets) \
    namespace {                                                                         \
    const ::bb::hook::Registrar BB_CONCAT(bb_replace_, __COUNTER__){                     \
        ::bb::hook::Replacement{(link_addr), reinterpret_cast<void*>(&(stub)), #stub,     \
                                (patched), (function_addr), &(continuation), (targets)}}; \
    }                                                                                   \
    static_assert(true)
