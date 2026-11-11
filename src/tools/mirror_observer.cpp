#include "tools/mirror_observer.hpp"

#include <algorithm>
#include <exception>
#include <string_view>
#include <vector>

#include "common/log.hpp"
#include "gpu/vk/memory_mirror.hpp"
#include "hle/kernel/dmem.hpp"

namespace bb::tools {

namespace {

using common::LogLevel;
constexpr std::string_view channel = "mirror";

}  // namespace

std::size_t MirrorObserver::ranges() const {
    const std::scoped_lock lock(mutex_);
    return bound_.size();
}

std::vector<MirrorObserver::Piece> MirrorObserver::take(std::uintptr_t low, std::uintptr_t high) {
    std::vector<Piece> pieces;
    auto it = bound_.upper_bound(low);
    if (it != bound_.begin()) {
        --it;
    }
    while (it != bound_.end() && it->first < high) {
        const std::uintptr_t start = it->first, end = start + it->second.length;
        const Range range = it->second;
        if (end <= low) {
            ++it;
            continue;
        }
        const std::uintptr_t cut_low = std::max(low, start), cut_high = std::min(high, end);
        it = bound_.erase(it);
        pieces.push_back({cut_low, cut_high - cut_low, range.offset + (cut_low - start)});
        if (start < cut_low) {
            bound_.emplace(start, Range{cut_low - start, range.offset});
        }
        if (cut_high < end) {
            it = bound_.emplace(cut_high, Range{end - cut_high, range.offset + (cut_high - start)})
                     .first;
            ++it;
        }
    }
    return pieces;
}

void MirrorObserver::mapped(std::uintptr_t address, std::uint64_t length, std::uint64_t offset) {
    if (offset >= hle::kernel::dmem::total_size) {
        return;  // flexible memory
    }
    try {
        if (mirror_.bind(address, length, offset)) {
            const std::scoped_lock lock(mutex_);
            bound_.insert_or_assign(address, Range{length, offset});
            ++binds_;
        }
    } catch (const std::exception& e) {
        common::log(LogLevel::error, channel, "bind {:#x}-{:#x} of offset {:#x}: {}", address,
                    address + length, offset, e.what());
    }
}

void MirrorObserver::unmapped(std::uintptr_t address, std::uint64_t length, std::uint64_t) {
    const std::scoped_lock lock(mutex_);
    for (const Piece& p : take(address, address + length)) {
        try {
            mirror_.unbind(p.address, p.length, p.offset);
            ++unbinds_;
        } catch (const std::exception& e) {
            common::log(LogLevel::error, channel, "unbind {:#x}-{:#x} of offset {:#x}: {}",
                        p.address, p.address + p.length, p.offset, e.what());
        }
    }
}

void MirrorObserver::released(std::uint64_t offset, std::uint64_t length) {
    const std::scoped_lock lock(mutex_);
    // The mirror drops what it holds of the range; the mappings stay, but are no longer bound.
    // Forget the parts of the records that view the range.
    std::vector<Piece> hit;
    for (const auto& [start, r] : bound_) {
        const std::uint64_t low = std::max(offset, r.offset),
                            high = std::min(offset + length, r.offset + r.length);
        if (low < high) {
            hit.push_back({start + (low - r.offset), high - low, low});
        }
    }
    for (const Piece& p : hit) {
        take(p.address, p.address + p.length);
    }
    try {
        // The mirror drops whole blocks: a neighbouring mapping that shares one loses its pages
        // there too, so forget them as well (a late unmap must not unbind them).
        for (const auto& [address, size] : mirror_.release(offset, length)) {
            take(address, address + size);
        }
        ++releases_;
    } catch (const std::exception& e) {
        common::log(LogLevel::error, channel, "release {:#x}-{:#x}: {}", offset, offset + length,
                    e.what());
    }
}

}  // namespace bb::tools
