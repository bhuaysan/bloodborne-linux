#include "hook/hook.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "loader/bytes.hpp"

namespace bb::hook {
namespace {

using Bytes = std::array<std::byte, patch_size>;

// A live hook: the bytes it replaced and the patch it wrote.
struct Saved {
    Bytes original, patch;
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
    for (const loader::Segment& s : image.segments) {
        if ((s.prot & PROT_EXEC) && link >= s.link_addr && link - s.link_addr < s.mem_size &&
            s.mem_size - (link - s.link_addr) >= patch_size) {
            return &s;
        }
    }
    return nullptr;
}

// The live hook at `link` of `image`, which must lie in a code segment. An entry whose patch is
// gone is left over from an image that was unmapped: it is dropped and counts as not hooked.
std::map<Key, Saved>::iterator live(const loader::Image& image, std::uint64_t link) {
    auto it = saved().find({image.base, link});
    if (it != saved().end() && read(image, link) != it->second.patch) {
        saved().erase(it);
        return saved().end();
    }
    return it;
}

// Checks the parts that apply and remove share. Returns the code segment of each entry.
std::vector<const loader::Segment*> check(const loader::Image& image,
                                          std::span<const Replacement> replaced) {
    std::vector<const loader::Segment*> segments;
    for (std::size_t i = 0; i < replaced.size(); ++i) {
        const Replacement& r = replaced[i];
        auto same = [&](const Replacement& other) { return other.link_addr == r.link_addr; };
        if (std::any_of(replaced.begin(), replaced.begin() + i, same)) {
            refuse(r, "double hook: the address is listed twice");
        }
        const loader::Segment* s = code_segment(image, r.link_addr);
        if (!s) {
            refuse(r, "the patch lies outside the executable segments");
        }
        segments.push_back(s);
    }
    return segments;
}

void protect(std::uintptr_t start, std::uintptr_t end, int prot) {
    if (mprotect(reinterpret_cast<void*>(start), end - start, prot) != 0) {
        int err = errno;
        throw std::system_error(err, std::generic_category(),
                                std::format("mprotect of {:#x}..{:#x}", start, end));
    }
}

// Writes `bytes` at `link`. On a finalized image, makes the pages writable for the write and
// gives them the protection of their segment `s` back.
void write(loader::Image& image, const loader::Segment& s, std::uint64_t link,
           const Bytes& bytes) {
    void* at = reinterpret_cast<void*>(image.base + link);
    if (!image.finalized()) {
        std::memcpy(at, bytes.data(), bytes.size());
        return;
    }
    auto page = static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
    std::uintptr_t start = (image.base + link) & ~(page - 1);
    std::uintptr_t end = (image.base + link + patch_size + page - 1) & ~(page - 1);
    protect(start, end, PROT_READ | PROT_WRITE);
    std::memcpy(at, bytes.data(), bytes.size());
    protect(start, end, s.prot);
}

}  // namespace

void apply(loader::Image& image, std::span<const Replacement> replaced) {
    std::vector<const loader::Segment*> segments = check(image, replaced);
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
        if (live(image, r.link_addr) != saved().end()) {
            refuse(r, "double hook: the address is already hooked");
        }
    }
    for (std::size_t i = 0; i < replaced.size(); ++i) {
        std::uint64_t link = replaced[i].link_addr;
        // Recorded first: if the write fails, live() finds no patch and drops the entry.
        Saved& hook = saved()[{image.base, link}];
        hook = Saved{read(image, link), patch_for(replaced[i].impl)};
        write(image, *segments[i], link, hook.patch);
    }
}

void remove(loader::Image& image, std::span<const Replacement> replaced) {
    std::vector<const loader::Segment*> segments = check(image, replaced);
    for (const Replacement& r : replaced) {
        if (live(image, r.link_addr) == saved().end()) {
            refuse(r, "not hooked");
        }
    }
    for (std::size_t i = 0; i < replaced.size(); ++i) {
        auto it = saved().find({image.base, replaced[i].link_addr});
        write(image, *segments[i], replaced[i].link_addr, it->second.original);
        saved().erase(it);
    }
}

}  // namespace bb::hook
