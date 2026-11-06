// The differential test harness: calls a game function and its C++ replacement from the same
// memory state and compares what they did (docs/design/runtime.md, "Differential test harness").
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "hle/hle.hpp"
#include "loader/image.hpp"

namespace bb::harness {

// The memory for test inputs, part of the snapshot and zero in the baseline. A bump allocator
// from offset 0 that starts over at every restore, so a builder that makes the same calls gets
// the same addresses every time.
class Arena {
public:
    static constexpr std::size_t size = 16 << 20;
    Arena();
    ~Arena();
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    std::byte* data() const { return data_; }
    // The next allocation starts at offset 0 again.
    void reset() { used_ = 0; }

    // `n` zeroed bytes aligned to `align` (a power of two). Throws std::length_error when full.
    void* alloc(std::size_t n, std::size_t align = 16);
    // `count` zeroed objects of T. Throws std::length_error when they don't fit.
    template <class T>
    T* zeroed(std::size_t count = 1) {
        static_assert(std::is_trivially_copyable_v<T> &&
                      std::is_trivially_default_constructible_v<T>);
        if (count > size / sizeof(T)) {  // also keeps sizeof(T) * count from wrapping
            throw std::length_error("harness: the arena is full");
        }
        return static_cast<T*>(alloc(sizeof(T) * count, alignof(T)));
    }
    // A copy of `s` with a terminating NUL.
    char* str(std::string_view s);
    // A copy of `b`.
    std::byte* bytes(std::span<const std::byte> b);

private:
    std::byte* data_;
    std::size_t used_ = 0;
};

// A block of memory that the harness restores and compares.
struct Region {
    std::string name;  // "image", "arena", "data block <library> <nid>" or the caller's
    std::byte* data;
    std::size_t size;
    std::uint64_t link;  // link address of data for an image segment, else 0
    bool is_image;       // reported as link addresses, else as offsets
    // For a shared mapping of a sparse file, such as direct memory: the file and the offset of
    // data in it. data, fd_offset and size are multiples of the host page size. The harness then
    // keeps and compares only the pages that are data in the file (the holes read zero), and the
    // restore punches holes where the baseline had them, so the cost follows the pages a test
    // touches.
    int fd = -1;
    std::uint64_t fd_offset = 0;
};

// State outside the regions that the harness puts back at every restore and compares between
// the two calls, such as the guest mappings and direct memory (tests/harness/load_set.cpp). Its
// baseline is its own, taken together with the harness's.
class State {
public:
    virtual ~State() = default;
    // The name in the report.
    virtual std::string_view name() const = 0;
    // Puts the baseline back. Called first by every Harness::restore(), before the regions.
    virtual void restore() = 0;
    // What changed since the baseline, one line each, in an order that doesn't depend on the
    // order of the calls that changed it. Compared line by line between the two calls.
    virtual std::vector<std::string> changes() const = 0;
};

// What one comparison found. True when nothing differs; report() then is empty.
class Result {
public:
    explicit operator bool() const { return report_.empty(); }
    const std::string& report() const { return report_; }
    void add(std::string_view line) {
        report_ += line;
        report_ += '\n';
    }

private:
    std::string report_;
};

// One call to a function stub while a harness was installed, with all six integer argument
// registers.
struct StubCall {
    const hle::StubInfo* stub;
    std::array<std::uint64_t, 6> args;
};

// Takes the baseline of `image` (mapped and relocated) on construction: its writable segments,
// the data blocks that bb::hle::resolve created so far, the `extra` regions and the arena, and
// restores and compares `state` (if given, it must outlive the harness) as well. Applies no hooks
// and runs no init code. Records the stub calls while it exists, so only one harness may
// exist at a time: throws std::logic_error if another stub observer (such as another harness) is
// installed, std::invalid_argument for a region with an fd that isn't page-aligned.
class Harness : private hle::StubObserver {
public:
    explicit Harness(loader::Image& image, std::vector<Region> extra = {},
                     State* state = nullptr);
    ~Harness() override;

    Arena& arena() { return arena_; }
    // Puts the baseline back into the state and every region, resets the arena and clears the
    // stub log.
    void restore();

    // The stub calls since the last restore().
    const std::vector<StubCall>& log() const { return log_; }
    // Compares the first `n` (0 to 6) arguments of the calls to this import. The default is 0.
    // Throws std::invalid_argument for other `n`.
    void declare_arity(std::string_view library, std::string_view nid, int n);

    // Calls the original at `link_addr` and `replacement` with the arguments `build(arena())`
    // returns (a std::tuple), each from the baseline, and compares the return values, every
    // region and the stub logs.
    template <class Sig, class Build>
    Result compare(std::uint64_t link_addr, Sig* replacement, Build&& build) {
        return compare_as(image_.fn<Sig>(link_addr), replacement, build);
    }
    // The same with the original as a function of this process, for tests of the harness with a
    // synthetic original that has the guest ABI.
    template <class Sig, class Build>
    Result compare_functions(Sig* original, Sig* replacement, Build&& build) {
        return compare_as(original, replacement, build);
    }

private:
    template <class R, class... A, class Build>
    Result compare_as(R (*original_fn)(A...), R (*replacement)(A...), Build& build) {
        auto run = [&](R (*f)(A...)) {
            restore();
            std::tuple<A...> args = build(arena_);
            if constexpr (std::is_void_v<R>) {
                std::apply(f, args);
                return 0;
            } else {
                return std::apply(f, args);
            }
        };
        const auto original = run(original_fn);
        keep_original();
        const auto replaced = run(replacement);
        Result result;
        if (!same(original, replaced)) {
            result.add(std::format("return value: original {}, replacement {}", show(original),
                                   show(replaced)));
        }
        compare_state(result);
        return result;
    }

    template <class T>
    static bool same(const T& a, const T& b) {
        if constexpr (std::is_floating_point_v<T>) {
            static_assert(sizeof(T) <= 8, "long double has padding bytes");
            return std::memcmp(&a, &b, sizeof(T)) == 0;  // bitwise: NaN payloads, -0
        } else {
            return a == b;
        }
    }

    template <class T>
    static std::string show(const T& v) {
        if constexpr (std::is_floating_point_v<T>) {
            std::uint64_t bits = 0;
            std::memcpy(&bits, &v, sizeof(T));
            return std::format("{} (bits {:#x})", v, bits);
        } else if constexpr (std::is_pointer_v<T>) {
            return std::format("{}", reinterpret_cast<const void*>(v));
        } else if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {
            return std::format("{:#x}", static_cast<std::uint64_t>(v));
        } else {
            return "(a value without a printer)";
        }
    }

    // The contents of one region: every byte, or for a region with an fd its data pages by
    // offset in the region.
    struct Contents {
        std::vector<std::byte> bytes;
        std::map<std::size_t, std::vector<std::byte>> pages;
    };

    void on_call(const hle::StubInfo& stub, const std::uint64_t args[6]) override;
    // Copies region i into `out`, reusing its buffers.
    void take(std::size_t i, Contents& out) const;
    void compare_region(std::size_t i, Result& result, std::size_t& ranges) const;
    // Saves the regions and the stub log after the original's call.
    void keep_original();
    // Compares the regions and the stub log after the replacement's call with the saved ones.
    void compare_state(Result& result) const;
    void compare_log(Result& result) const;
    void compare_changes(Result& result) const;

    loader::Image& image_;
    State* state_;
    Arena arena_;
    std::vector<Region> regions_;
    std::vector<Contents> baseline_, after_original_;
    std::vector<StubCall> log_, original_log_;
    std::vector<std::string> original_changes_;  // of state_ after the original's call
    std::unordered_map<std::string, int> arity_;  // by hle::import_key
    // The start and end of the stack of the thread that last ran compare_log, and that thread.
    mutable std::thread::id stack_thread_;
    mutable std::pair<std::uintptr_t, std::uintptr_t> stack_{};
};

}  // namespace bb::harness
