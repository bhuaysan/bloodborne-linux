#include "harness/harness.hpp"

#include <pthread.h>
#include <sys/mman.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace bb::harness {
namespace {

constexpr std::size_t page = 4096, max_ranges = 8, max_shown_bytes = 8, max_shown_calls = 16;

std::string show_bytes(const std::byte* p, std::size_t n) {
    std::string out;
    for (std::size_t i = 0; i < std::min(n, max_shown_bytes); ++i) {
        out += std::format("{}{:02x}", i ? " " : "", static_cast<unsigned>(p[i]));
    }
    return n > max_shown_bytes ? out + " ..." : out;
}

std::string arity_key(std::string_view library, std::string_view nid) {
    std::string k(library);
    k += '\0';
    k += nid;
    return k;
}

// The start and end of the calling thread's stack.
std::pair<std::uintptr_t, std::uintptr_t> thread_stack() {
    pthread_attr_t attr;
    if (const int err = pthread_getattr_np(pthread_self(), &attr); err != 0) {
        throw std::system_error(err, std::generic_category(), "harness: pthread_getattr_np");
    }
    void* addr = nullptr;
    std::size_t size = 0;
    const int err = pthread_attr_getstack(&attr, &addr, &size);
    pthread_attr_destroy(&attr);
    if (err != 0) {
        throw std::system_error(err, std::generic_category(), "harness: pthread_attr_getstack");
    }
    const auto start = reinterpret_cast<std::uintptr_t>(addr);
    return {start, start + size};
}

// A stub argument as it is compared.
struct Normalized {
    enum class Kind { raw, region, stack } kind;
    std::size_t region;   // for Kind::region
    std::uint64_t value;  // the offset in the region, or the raw value
    bool operator==(const Normalized&) const = default;
};

void show_log(Result& result, std::string_view side, const std::vector<StubCall>& log) {
    for (std::size_t i = 0; i < std::min(log.size(), max_shown_calls); ++i) {
        const StubCall& c = log[i];
        result.add(std::format("  {} {}: {} {} ({}) {:#x} {:#x} {:#x} {:#x} {:#x} {:#x}", side,
                               i + 1, c.stub->library, c.stub->nid, c.stub->name, c.args[0],
                               c.args[1], c.args[2], c.args[3], c.args[4], c.args[5]));
    }
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
    for (const hle::DataBlock& b : hle::data_blocks()) {
        regions_.push_back({std::format("data block {} {}", b.library, b.nid), b.data,
                            hle::data_block_size, 0, false});
    }
    regions_.push_back({"arena", arena_.data(), Arena::size, 0, false});
    for (const Region& r : regions_) {
        baseline_.emplace_back(r.data, r.data + r.size);
    }
    if (hle::StubObserver* previous = hle::set_stub_observer(this)) {
        hle::set_stub_observer(previous);
        throw std::logic_error("harness: another stub observer is installed");
    }
}

Harness::~Harness() {
    hle::set_stub_observer(nullptr);
}

void Harness::declare_arity(std::string_view library, std::string_view nid, int n) {
    if (n < 0 || n > 6) {
        throw std::invalid_argument(std::format("harness: arity {} is not 0 to 6", n));
    }
    arity_[arity_key(library, nid)] = n;
}

void Harness::on_call(const hle::StubInfo& stub, const std::uint64_t args[6]) {
    StubCall& call = log_.emplace_back(&stub);
    std::copy_n(args, 6, call.args.begin());
}

void Harness::restore() {
    for (std::size_t i = 0; i < regions_.size(); ++i) {
        std::memcpy(regions_[i].data, baseline_[i].data(), regions_[i].size);
    }
    arena_.reset();
    log_.clear();
}

void Harness::keep_original() {
    // Sized on the first compare and reused after: the regions never change.
    after_original_.resize(regions_.size());
    for (std::size_t i = 0; i < regions_.size(); ++i) {
        after_original_[i].resize(regions_[i].size);
        std::memcpy(after_original_[i].data(), regions_[i].data, regions_[i].size);
    }
    original_log_ = log_;
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
    compare_log(result);
}

void Harness::compare_log(Result& result) const {
    if (stack_thread_ != std::this_thread::get_id()) {
        stack_ = thread_stack();
        stack_thread_ = std::this_thread::get_id();
    }
    const auto [stack_start, stack_end] = stack_;
    auto normalize = [&](std::uint64_t v) -> Normalized {
        for (std::size_t i = 0; i < regions_.size(); ++i) {
            const auto start = reinterpret_cast<std::uintptr_t>(regions_[i].data);
            if (v >= start && v - start < regions_[i].size) {
                return {Normalized::Kind::region, i, v - start};
            }
        }
        if (v >= stack_start && v < stack_end) {
            return {Normalized::Kind::stack, 0, 0};  // "a stack pointer"
        }
        return {Normalized::Kind::raw, 0, v};
    };
    auto same = [&](const StubCall& a, const StubCall& b) {
        if (a.stub != b.stub) {
            return false;
        }
        const auto it = arity_.find(arity_key(a.stub->library, a.stub->nid));
        const int arity = it == arity_.end() ? 0 : it->second;
        for (int i = 0; i < arity; ++i) {
            if (normalize(a.args[i]) != normalize(b.args[i])) {
                return false;
            }
        }
        return true;
    };
    const std::vector<StubCall>& orig = original_log_;
    const std::vector<StubCall>& repl = log_;
    std::size_t n = 0;
    while (n < orig.size() && n < repl.size() && same(orig[n], repl[n])) {
        ++n;
    }
    if (n == orig.size() && n == repl.size()) {
        return;
    }
    result.add(std::format("stub log: differs at call {} (original {} calls, replacement {})",
                           n + 1, orig.size(), repl.size()));
    show_log(result, "original", orig);
    show_log(result, "replacement", repl);
}

}  // namespace bb::harness
