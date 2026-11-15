#include "hook/hook.hpp"

#include <sys/mman.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/memory.hpp"
#include "loader/bytes.hpp"

namespace bb::hook {
namespace {

using Bytes = std::array<std::byte, patch_size>;

// An executable page with a trampoline, unmapped with the hook.
struct Unmap {
    void operator()(void* page) const { ::munmap(page, common::page_size()); }
};
using Trampoline = std::unique_ptr<void, Unmap>;

// A live hook: the bytes it replaced, the patch it wrote and, for a wrapped function, the
// trampoline to its original.
struct Saved {
    Bytes original, patch;
    Trampoline trampoline;
    std::size_t span = patch_size;  // the bytes that must stay as they are: patch or prologue
};

using Key = std::pair<std::uintptr_t, std::uint64_t>;  // image base, link address

std::map<Key, Saved>& saved() {
    static std::map<Key, Saved> hooks;
    return hooks;
}

[[noreturn]] void refuse(const Replacement& r, std::string_view why) {
    throw HookError(
        std::format("hook {} at {:#x}: {}", r.name ? r.name : "?", r.link_addr, why));
}

Bytes patch_for(void* impl) {
    Bytes patch{std::byte{0xff}, std::byte{0x25}};  // jmp [rip+0], displacement 0
    auto target = reinterpret_cast<std::uintptr_t>(impl);
    std::memcpy(patch.data() + 6, &target, sizeof target);
    return patch;
}

Bytes read(const loader::Image& image, std::uint64_t link) {
    Bytes bytes;
    std::memcpy(bytes.data(), reinterpret_cast<const void*>(image.base + link), patch_size);
    return bytes;
}

// The executable segment that holds the whole patch at `link`, if any.
const loader::Segment* code_segment(const loader::Image& image, std::uint64_t link) {
    return image.segment_at(link, patch_size, PROT_EXEC);
}

// The live hook at `link` of `image`. An entry whose patch is gone, or no longer lies in a code
// segment, is left over from an image that was unmapped: it is dropped and counts as not hooked.
std::map<Key, Saved>::iterator live(const loader::Image& image, std::uint64_t link) {
    auto it = saved().find({image.base, link});
    if (it != saved().end() &&
        (!code_segment(image, link) || read(image, link) != it->second.patch)) {
        saved().erase(it);
        return saved().end();
    }
    return it;
}

// Checks the parts that apply and remove share. A patch it passes lies in a code segment.
void check(const loader::Image& image, std::span<const Replacement> replaced) {
    // Refuses the first entry that overlaps an earlier one, as a check against every earlier
    // entry would. If any earlier entry overlaps, the nearest one below or above does.
    std::set<std::uint64_t> earlier;
    for (const Replacement& r : replaced) {
        auto above = earlier.lower_bound(r.link_addr);
        if ((above != earlier.end() && *above - r.link_addr < patch_size) ||
            (above != earlier.begin() && r.link_addr - *std::prev(above) < patch_size)) {
            refuse(r, "double hook: the patch overlaps another entry of the list");
        }
        if (!code_segment(image, r.link_addr)) {
            refuse(r, "the patch lies outside the executable segments");
        }
        earlier.insert(r.link_addr);
    }
}

void protect(std::uintptr_t start, std::uintptr_t end, int prot) {
    common::protect(reinterpret_cast<void*>(start), end - start, prot);
}

// Writes `bytes` at `link`, which check() found in a code segment. On a finalized image, makes
// the pages writable for the write and gives them the protection of that segment back.
void write(loader::Image& image, std::uint64_t link, const Bytes& bytes) {
    void* at = reinterpret_cast<void*>(image.base + link);
    if (!image.finalized()) {
        std::memcpy(at, bytes.data(), bytes.size());
        return;
    }
    std::uintptr_t start = common::page_down(image.base + link);
    std::uintptr_t end = common::page_up(image.base + link + patch_size);
    protect(start, end, PROT_READ | PROT_WRITE);
    std::memcpy(at, bytes.data(), bytes.size());
    protect(start, end, code_segment(image, link)->prot);
}

// The prologue of `r`, then jmp [rip+0] to the instruction behind it, in a page of its own.
Trampoline make_trampoline(const loader::Image& image, const Replacement& r) {
    void* page = ::mmap(nullptr, common::page_size(), PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        refuse(r, "no memory for the trampoline");
    }
    Trampoline out(page);
    auto* code = static_cast<std::byte*>(page);
    std::memcpy(code, r.prologue.data(), r.prologue.size());
    Bytes jump = patch_for(reinterpret_cast<void*>(image.base + r.link_addr + r.prologue.size()));
    std::memcpy(code + r.prologue.size(), jump.data(), jump.size());
    protect(reinterpret_cast<std::uintptr_t>(page),
            reinterpret_cast<std::uintptr_t>(page) + common::page_size(), PROT_READ | PROT_EXEC);
    return out;
}

std::vector<Replacement>& registry() {
    static std::vector<Replacement> registered;
    return registered;
}

}  // namespace

std::span<const Replacement> replacements() {
    return registry();
}

void register_replacement(const Replacement& r) {
    registry().push_back(r);
}

void apply(loader::Image& image, std::span<const Replacement> replaced) {
    check(image, replaced);
    for (const Replacement& r : replaced) {
        if (!r.impl) {
            refuse(r, "the replacement is null");
        }
        std::optional<std::uint64_t> size;
        try {
            size = image.function_size(r.link_addr);
        } catch (const loader::LoadError& e) {
            refuse(r, e.what());
        }
        if (!size) {
            refuse(r, "not an FDE start");
        }
        if (*size < patch_size) {
            refuse(r, std::format("the function is {} bytes, a hook needs {}", *size, patch_size));
        }
        if (!r.prologue.empty()) {
            const std::size_t n = r.prologue.size();
            if (n < patch_size || n > max_prologue || n > *size) {
                refuse(r, std::format("a prologue of {} bytes, it needs {} to {} within the "
                                      "function's {}",
                                      n, patch_size, max_prologue, *size));
            }
            if (std::memcmp(reinterpret_cast<const void*>(image.base + r.link_addr),
                            r.prologue.data(), n) != 0) {
                refuse(r, "the prologue differs from the bytes in memory");
            }
        }
        // A live hook covers its patch and, with a prologue, the bytes its trampoline jumps
        // back behind; this one must not overlap that, nor its own span another hook's patch.
        const std::uint64_t span = std::max(patch_size, r.prologue.size());
        const std::uint64_t first =
            r.link_addr - std::min(r.link_addr, std::uint64_t{max_prologue - 1});
        for (std::uint64_t at = first; at < r.link_addr + span; ++at) {
            auto it = live(image, at);
            if (it != saved().end() && at + it->second.span > r.link_addr) {
                refuse(r, std::format("double hook: the patch overlaps the hook at {:#x}", at));
            }
        }
    }
    // Trampolines first, so a failure there patches nothing.
    std::vector<Trampoline> trampolines;
    for (const Replacement& r : replaced) {
        trampolines.push_back(r.prologue.empty() ? Trampoline{} : make_trampoline(image, r));
    }
    for (std::size_t i = 0; i < replaced.size(); ++i) {
        const Replacement& r = replaced[i];
        // Recorded first: if the write fails, live() finds no patch and drops the entry.
        Saved& hook = saved()[{image.base, r.link_addr}];
        hook = Saved{read(image, r.link_addr), patch_for(r.impl), std::move(trampolines[i]),
                     std::max(patch_size, r.prologue.size())};
        write(image, r.link_addr, hook.patch);
    }
}

void remove(loader::Image& image, std::span<const Replacement> replaced) {
    check(image, replaced);
    for (const Replacement& r : replaced) {
        if (live(image, r.link_addr) == saved().end()) {
            refuse(r, "not hooked");
        }
    }
    for (const Replacement& r : replaced) {
        auto it = saved().find({image.base, r.link_addr});
        write(image, r.link_addr, it->second.original);
        saved().erase(it);
    }
}

void* original(const loader::Image& image, std::uint64_t link) {
    auto it = live(image, link);
    if (it == saved().end()) {
        return reinterpret_cast<void*>(image.base + link);
    }
    if (!it->second.trampoline) {
        throw HookError(std::format("hook at {:#x}: no prologue, so no original", link));
    }
    return it->second.trampoline.get();
}

}  // namespace bb::hook
