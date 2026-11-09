// A fast 64-bit hash of guest shader code, to notice that the code behind a cached pipeline
// changed (docs/design/graphics.md, "Code hash of a cached pipeline"). Not cryptographic: it
// only has to tell two versions of a program apart by accident, not against an adversary.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace bb::common {

// Equal dwords give equal hashes; the length is part of the hash.
std::uint64_t hash_code(std::span<const std::uint32_t> words);

// The most code a cache entry that failed to translate is stamped with: its extent is that of the
// instructions that decode (gcn::readable_extent), at most the first 64 KiB.
inline constexpr std::size_t error_stamp_dwords = 16384;

// The identity of the first `dwords` dwords of some guest code. A cache entry keeps one per
// piece of code it was made from and is used only while the code at its address still matches.
struct CodeStamp {
    std::uint32_t dwords = 0;  // 0: no code, nothing to match
    std::uint64_t hash = 0;

    // The stamp of the first `dwords` dwords of `code`, or of all of it when it is shorter.
    static CodeStamp of(std::span<const std::uint32_t> code, std::size_t dwords) {
        dwords = dwords < code.size() ? dwords : code.size();
        return {static_cast<std::uint32_t>(dwords),
                dwords != 0 ? hash_code(code.first(dwords)) : 0};
    }
    // Whether `code` (now readable at the address) still has the stamped dwords. Unreadable or
    // shortened code doesn't match.
    bool matches(std::span<const std::uint32_t> code) const {
        return dwords == 0 || (code.size() >= dwords && hash_code(code.first(dwords)) == hash);
    }
};

// Guest code for one lookup that is compared with several stamps. It hashes a prefix once and
// reuses the hash for every stamp of that length.
class CodeProbe {
public:
    explicit CodeProbe(std::span<const std::uint32_t> code) : code_(code) {}

    std::span<const std::uint32_t> code() const { return code_; }
    // Same answer as stamp.matches(code()).
    bool matches(const CodeStamp& stamp) {
        if (stamp.dwords == 0) return true;
        if (code_.size() < stamp.dwords) return false;
        if (stamp.dwords != hashed_dwords_) {
            hashed_dwords_ = stamp.dwords;
            hash_ = hash_code(code_.first(stamp.dwords));
        }
        return hash_ == stamp.hash;
    }

private:
    std::span<const std::uint32_t> code_;
    std::uint32_t hashed_dwords_ = 0;  // 0: nothing hashed yet
    std::uint64_t hash_ = 0;
};

}  // namespace bb::common
