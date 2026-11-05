#include "harness/harness.hpp"

#include <sys/mman.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace bb::harness {
namespace {

constexpr std::size_t page = 4096, max_ranges = 8, max_shown_bytes = 8;

std::string show_bytes(const std::byte* p, std::size_t n) {
    std::string out;
    for (std::size_t i = 0; i < std::min(n, max_shown_bytes); ++i) {
        out += std::format("{}{:02x}", i ? " " : "", static_cast<unsigned>(p[i]));
    }
    return n > max_shown_bytes ? out + " ..." : out;
}

}  // namespace

Arena::Arena() {
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        throw std::system_error(errno, std::generic_category(), "harness: arena mmap");
    }
    data_ = static_cast<std::byte*>(p);
}

Arena::~Arena() {
    munmap(data_, size);
}

void* Arena::alloc(std::size_t n, std::size_t align) {
    const std::size_t start = (used_ + align - 1) & ~(align - 1);
    if (start < used_ || start > size || n > size - start) {
        throw std::length_error("harness: the arena is full");
    }
    used_ = start + n;
    std::memset(data_ + start, 0, n);
    return data_ + start;
}

char* Arena::str(std::string_view s) {
    auto* p = static_cast<char*>(alloc(s.size() + 1, 1));
    if (!s.empty()) {  // an empty view's data() may be null, which memcpy must not get
        std::memcpy(p, s.data(), s.size());
    }
    return p;
}

std::byte* Arena::bytes(std::span<const std::byte> b) {
    auto* p = static_cast<std::byte*>(alloc(b.size()));
    if (!b.empty()) {
        std::memcpy(p, b.data(), b.size());
    }
    return p;
}

Harness::Harness(loader::Image& image) : image_(image) {
    for (const loader::Segment& s : image.segments) {
        if (s.prot & PROT_WRITE) {
            regions_.push_back({"image", reinterpret_cast<std::byte*>(image.runtime(s.link_addr)),
                                s.mem_size, s.link_addr, true});
        }
    }
    regions_.push_back({"arena", arena_.data(), Arena::size, 0, false});
    for (const Region& r : regions_) {
        baseline_.emplace_back(r.data, r.data + r.size);
    }
}

void Harness::restore() {
    for (std::size_t i = 0; i < regions_.size(); ++i) {
        std::memcpy(regions_[i].data, baseline_[i].data(), regions_[i].size);
    }
    arena_.reset();
}

void Harness::keep_original() {
    after_original_.clear();
    for (const Region& r : regions_) {
        after_original_.emplace_back(r.data, r.data + r.size);
    }
}

void Harness::compare_state(Result& result) const {
    std::size_t ranges = 0;
    for (std::size_t i = 0; i < regions_.size(); ++i) {
        const Region& r = regions_[i];
        const std::byte *orig = after_original_[i].data(), *repl = r.data;
        for (std::size_t at = 0; at < r.size;) {
            if (at % page == 0 && r.size - at >= page &&
                std::memcmp(orig + at, repl + at, page) == 0) {
                at += page;
                continue;
            }
            if (orig[at] == repl[at]) {
                ++at;
                continue;
            }
            const std::size_t start = at;
            while (at < r.size && orig[at] != repl[at]) {
                ++at;
            }
            if (++ranges <= max_ranges) {
                // Link addresses for the image, offsets for the others.
                const std::string_view sign = r.is_image ? "" : "+";
                result.add(std::format("memory: {} {}{:#x}..{}{:#x} differs: original {}, "
                                       "replacement {}",
                                       r.name, sign, r.link + start, sign, r.link + at,
                                       show_bytes(orig + start, at - start),
                                       show_bytes(repl + start, at - start)));
            }
        }
    }
    if (ranges > max_ranges) {
        result.add(std::format("memory: {} differing ranges in all", ranges));
    }
}

}  // namespace bb::harness
