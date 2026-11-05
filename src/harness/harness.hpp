// The differential test harness: calls a game function and its C++ replacement from the same
// memory state and compares what they did (docs/design/runtime.md, "Differential test harness").
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

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
    // `count` zeroed objects of T.
    template <class T>
    T* zeroed(std::size_t count = 1) {
        static_assert(std::is_trivially_copyable_v<T> &&
                      std::is_trivially_default_constructible_v<T>);
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
    std::string name;  // "image" or "arena"
    std::byte* data;
    std::size_t size;
    std::uint64_t link;  // link address of data for an image segment, else 0
    bool is_image;       // reported as link addresses, else as offsets
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

// Takes the baseline of `image` (mapped and relocated) on construction: its writable segments
// and the arena. Applies no hooks and runs no init code.
class Harness {
public:
    explicit Harness(loader::Image& image);

    Arena& arena() { return arena_; }
    const std::vector<Region>& regions() const { return regions_; }
    // Puts the baseline back into every region and resets the arena.
    void restore();

    // Calls the original at `link_addr` and `replacement` with the arguments `build(arena())`
    // returns (a std::tuple), each from the baseline, and compares the return values and every
    // region.
    template <class Sig, class Build>
    Result compare(std::uint64_t link_addr, Sig* replacement, Build&& build) {
        return compare_as(link_addr, replacement, build, std::type_identity<Sig>{});
    }

private:
    template <class R, class... A, class Build>
    Result compare_as(std::uint64_t link_addr, R (*replacement)(A...), Build& build,
                      std::type_identity<R(A...)>) {
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
        const auto original = run(image_.fn<R(A...)>(link_addr));
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

    // Saves the regions after the original's call.
    void keep_original();
    // Compares the regions after the replacement's call with the saved ones.
    void compare_state(Result& result) const;

    loader::Image& image_;
    Arena arena_;
    std::vector<Region> regions_;
    std::vector<std::vector<std::byte>> baseline_, after_original_;
};

}  // namespace bb::harness
