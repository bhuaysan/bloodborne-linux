#include "harness/harness.hpp"

#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iterator>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "common/memory.hpp"

namespace bb::harness {
namespace {

constexpr std::size_t max_ranges = 8, max_shown_bytes = 8, max_shown_calls = 16;

std::string show_bytes(const std::byte* p, std::size_t n) {
    std::string out;
    for (std::size_t i = 0; i < std::min(n, max_shown_bytes); ++i) {
        out += std::format("{}{:02x}", i ? " " : "", static_cast<unsigned>(p[i]));
    }
    return n > max_shown_bytes ? out + " ..." : out;
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

// The data in the file of `r` (Region::fd) as page-aligned [start, end) offsets in `r`, in order.
// The rest are holes, which read zero.
std::vector<std::pair<std::size_t, std::size_t>> data_ranges(const Region& r) {
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    const auto start = static_cast<off_t>(r.fd_offset), end = start + static_cast<off_t>(r.size);
    auto fail = [&] {
        throw std::system_error(errno, std::generic_category(), "harness: lseek " + r.name);
    };
    for (off_t at = start; at < end;) {
        const off_t data = lseek(r.fd, at, SEEK_DATA);
        if (data < 0 && errno == ENXIO) {  // no data after `at`
            break;
        }
        if (data < 0) {
            fail();
        }
        if (data >= end) {
            break;
        }
        const off_t hole = lseek(r.fd, data, SEEK_HOLE);
        if (hole < 0) {
            fail();
        }
        const auto page = static_cast<off_t>(common::page_size());
        ranges.emplace_back((data & ~(page - 1)) - start,
                            ((std::min(hole, end) + page - 1) & ~(page - 1)) - start);
        at = hole;
    }
    return ranges;
}

// Makes [at, at + length) of `r` a hole in its file, so it reads zero and holds no memory.
void punch(const Region& r, std::size_t at, std::size_t length) {
    if (length != 0 && fallocate(r.fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                                 static_cast<off_t>(r.fd_offset + at),
                                 static_cast<off_t>(length)) != 0) {
        throw std::system_error(errno, std::generic_category(), "harness: fallocate " + r.name);
    }
}

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
    data_ = static_cast<std::byte*>(
        common::map_anonymous(size, PROT_READ | PROT_WRITE, "harness: arena mmap"));
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

Harness::Harness(loader::Image& image, std::vector<Region> extra, State* state)
    : image_(image), state_(state) {
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
    std::ranges::move(extra, std::back_inserter(regions_));
    regions_.push_back({"arena", arena_.data(), Arena::size, 0, false});
    const std::size_t page = common::page_size();
    for (const Region& r : regions_) {
        if (r.fd >= 0 && ((reinterpret_cast<std::uintptr_t>(r.data) | r.fd_offset | r.size) &
                          (page - 1)) != 0) {
            throw std::invalid_argument(
                std::format("harness: region {} is not aligned to the page size", r.name));
        }
    }
    baseline_.resize(regions_.size());
    for (std::size_t i = 0; i < regions_.size(); ++i) {
        take(i, baseline_[i]);
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
    arity_[hle::import_key(library, nid)] = n;
}

void Harness::on_call(const hle::StubInfo& stub, const std::uint64_t args[6]) {
    StubCall& call = log_.emplace_back(&stub);
    std::copy_n(args, 6, call.args.begin());
}

void Harness::restore() {
    if (state_ != nullptr) {
        state_->restore();
    }
    const std::size_t page = common::page_size();
    for (std::size_t i = 0; i < regions_.size(); ++i) {
        const Region& r = regions_[i];
        const Contents& b = baseline_[i];
        if (r.fd < 0) {
            std::memcpy(r.data, b.bytes.data(), r.size);
            continue;
        }
        // Holes where the baseline had them, then its data pages back.
        for (const auto& [start, end] : data_ranges(r)) {
            std::size_t hole = start;
            for (std::size_t at = start; at <= end; at += page) {
                if (at == end || b.pages.contains(at)) {
                    punch(r, hole, at - hole);
                    hole = at + page;
                }
            }
        }
        for (const auto& [at, bytes] : b.pages) {
            std::memcpy(r.data + at, bytes.data(), page);
        }
    }
    arena_.reset();
    log_.clear();
}

void Harness::take(std::size_t i, Contents& out) const {
    const Region& r = regions_[i];
    if (r.fd < 0) {
        out.bytes.resize(r.size);  // sized once and reused after: the regions never change
        std::memcpy(out.bytes.data(), r.data, r.size);
        return;
    }
    const std::size_t page = common::page_size();
    out.pages.clear();
    for (const auto& [start, end] : data_ranges(r)) {
        for (std::size_t at = start; at < end; at += page) {
            out.pages[at].assign(r.data + at, r.data + at + page);
        }
    }
}

void Harness::keep_original() {
    after_original_.resize(regions_.size());
    for (std::size_t i = 0; i < regions_.size(); ++i) {
        take(i, after_original_[i]);
    }
    original_log_ = log_;
    if (state_ != nullptr) {
        original_changes_ = state_->changes();
    }
}

void Harness::compare_state(Result& result) const {
    std::size_t ranges = 0;
    for (std::size_t i = 0; i < regions_.size(); ++i) {
        compare_region(i, result, ranges);
    }
    if (ranges > max_ranges) {
        result.add(std::format("memory: {} differing ranges in all", ranges));
    }
    compare_log(result);
    compare_changes(result);
}

void Harness::compare_changes(Result& result) const {
    if (state_ == nullptr) {
        return;
    }
    const std::vector<std::string>& orig = original_changes_;
    const std::vector<std::string> repl = state_->changes();
    const auto [o, r] = std::ranges::mismatch(orig, repl);
    if (o == orig.end() && r == repl.end()) {
        return;
    }
    result.add(std::format("{}: differs at change {} (original {} changes, replacement {})",
                           state_->name(), o - orig.begin() + 1, orig.size(), repl.size()));
    auto show = [&](std::string_view side, const std::vector<std::string>& lines) {
        for (std::size_t i = 0; i < std::min(lines.size(), max_shown_calls); ++i) {
            result.add(std::format("  {} {}: {}", side, i + 1, lines[i]));
        }
    };
    show("original", orig);
    show("replacement", repl);
}

void Harness::compare_region(std::size_t i, Result& result, std::size_t& ranges) const {
    const Region& r = regions_[i];
    const Contents& o = after_original_[i];
    const std::size_t page = common::page_size();
    // The pages to compare, in order: all of them, or those that are data on either side.
    std::vector<std::size_t> pages, now;
    if (r.fd < 0) {
        for (std::size_t at = 0; at < r.size; at += page) {
            pages.push_back(at);
        }
    } else {
        for (const auto& [start, end] : data_ranges(r)) {
            for (std::size_t at = start; at < end; at += page) {
                now.push_back(at);
            }
        }
        std::ranges::set_union(now, o.pages | std::views::keys, std::back_inserter(pages));
    }
    static const std::vector<std::byte> zeros(page);
    auto original = [&](std::size_t at) {
        if (r.fd < 0) {
            return o.bytes.data() + at;
        }
        const auto it = o.pages.find(at);
        return it == o.pages.end() ? zeros.data() : it->second.data();
    };
    auto replacement = [&](std::size_t at) -> const std::byte* {
        // A hole reads zero, and reading it through the mapping would allocate a page.
        return r.fd < 0 || std::ranges::binary_search(now, at) ? r.data + at : zeros.data();
    };
    auto shown = [&](auto& side, std::size_t start, std::size_t end) {
        std::array<std::byte, max_shown_bytes> bytes{};
        for (std::size_t k = 0; k < std::min(end - start, max_shown_bytes); ++k) {
            const std::size_t at = start + k;
            bytes[k] = side(at - at % page)[at % page];
        }
        return show_bytes(bytes.data(), end - start);
    };
    std::optional<std::size_t> open;  // the start of the differing range being read
    std::size_t done = 0;             // the end of the last page compared
    auto close = [&](std::size_t end) {
        if (++ranges <= max_ranges) {
            // Link addresses for the image, offsets for the others.
            const std::string_view sign = r.is_image ? "" : "+";
            result.add(std::format("memory: {} {}{:#x}..{}{:#x} differs: original {}, "
                                   "replacement {}",
                                   r.name, sign, r.link + *open, sign, r.link + end,
                                   shown(original, *open, end), shown(replacement, *open, end)));
        }
        open.reset();
    };
    for (const std::size_t at : pages) {
        if (open && at != done) {
            close(done);
        }
        const std::size_t n = std::min(page, r.size - at);
        const std::byte *a = original(at), *b = replacement(at);
        done = at + n;
        if (!open && std::memcmp(a, b, n) == 0) {
            continue;
        }
        for (std::size_t k = 0; k < n; ++k) {
            if ((a[k] != b[k]) != open.has_value()) {
                if (open) {
                    close(at + k);
                } else {
                    open = at + k;
                }
            }
        }
    }
    if (open) {
        close(done);
    }
}

void Harness::compare_log(Result& result) const {
    // A thread id can be reused after its thread exits, so the cached bounds also have to
    // contain the current stack.
    const char local = 0;
    const auto here = reinterpret_cast<std::uintptr_t>(&local);
    if (stack_thread_ != std::this_thread::get_id() || here < stack_.first ||
        here >= stack_.second) {
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
        const auto it = arity_.find(hle::import_key(a.stub->library, a.stub->nid));
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
