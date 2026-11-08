// Keeps the GPU memory mirror in step with the direct memory mappings of the game
// (docs/design/graphics.md, "GPU memory mirror", item 21).
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

#include "hle/kernel/vmem.hpp"

namespace bb::vk {
class MemoryMirror;
}

namespace bb::tools {

// Every direct mapping is mirrored, whatever its protection bits, until a run shows which
// mappings the game makes with those bits. MemoryMirror::unbind() takes only ranges that bind()
// bound, so the observer records what was bound and unbinds only that: a mapping that bind()
// refused (outside the mirror) or that threw, and a part that released() took, are never
// unbound. Failures are logged and don't reach the guest.
class MirrorObserver final : public hle::kernel::vmem::DirectObserver {
public:
    // The mirror must outlive the observer.
    explicit MirrorObserver(vk::MemoryMirror& mirror) : mirror_(mirror) {}

    void mapped(std::uintptr_t address, std::uint64_t length, std::uint64_t offset) override;
    void unmapped(std::uintptr_t address, std::uint64_t length, std::uint64_t offset) override;
    void released(std::uint64_t offset, std::uint64_t length) override;

    // How many times each call reached the mirror (a bind() that refused counts as no bind).
    std::uint64_t binds() const { return binds_; }
    std::uint64_t unbinds() const { return unbinds_; }
    std::uint64_t releases() const { return releases_; }
    // The number of recorded bound ranges, for tests.
    std::size_t ranges() const;

private:
    struct Range {
        std::uint64_t length;
        std::uint64_t offset;  // in the memfd
    };
    // A bound range, to take out of the records.
    struct Piece {
        std::uintptr_t address;
        std::uint64_t length;
        std::uint64_t offset;
    };
    // Removes the recorded ranges' parts inside [low, high) of the window and returns them.
    // Needs mutex_.
    std::vector<Piece> take(std::uintptr_t low, std::uintptr_t high);

    vk::MemoryMirror& mirror_;
    mutable std::mutex mutex_;
    std::map<std::uintptr_t, Range> bound_;  // by address, never overlapping
    std::atomic<std::uint64_t> binds_{0}, unbinds_{0}, releases_{0};
};

}  // namespace bb::tools
