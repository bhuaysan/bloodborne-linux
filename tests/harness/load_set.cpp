#include "harness/load_set.hpp"

#include <sys/mman.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "game/image.hpp"
#include "hle/hle.hpp"
#include "hle/kernel/abi.hpp"
#include "hle/kernel/dmem.hpp"
#include "hle/kernel/tls.hpp"
#include "hle/kernel/vmem.hpp"
#include "hle/link.hpp"
#include "hle/prx_init.hpp"
#include "loader/relocate.hpp"
#include "support/game.hpp"
#include "support/leaks.hpp"

namespace bb::test {

namespace {

using harness::Region;
namespace vmem = hle::kernel::vmem;

// libc's malloc and free, which libc.prx exports (symbols/nids.csv).
constexpr std::string_view malloc_nid = "gQX+4GDQjpM", free_nid = "tIhsqj0qsFE";

template <class F>
F* libc_export(const loader::LoadSet& set, std::string_view nid) {
    for (const loader::LoadedModule& m : set.modules) {
        if (m.file != "libc.prx") {
            continue;
        }
        for (const loader::Export& e : loader::exports(m.image)) {
            if (e.library == "libc" && e.nid == nid) {
                return reinterpret_cast<F*>(e.address);
            }
        }
    }
    throw std::runtime_error(std::format("harness: libc.prx exports no {}", nid));
}

std::byte* bytes_at(std::uintptr_t address) {
    return reinterpret_cast<std::byte*>(address);
}

// The state the PRX init, the heap and the calling thread's TLS add to what Harness(Image&)
// covers (docs/design/m3-core-os.md, Memory, "Harness").
std::vector<Region> load_set_regions(const loader::LoadSet& set) {
    std::vector<Region> regions;
    auto add = [&](Region r) { regions.push_back(std::move(r)); };
    for (const loader::LoadedModule& m : set.modules) {
        if (m.id == loader::eboot_module_id) {
            continue;  // the harness takes the eboot's writable segments itself
        }
        for (const loader::Segment& s : m.image.segments) {
            if ((s.prot & PROT_WRITE) != 0) {
                add({m.file, bytes_at(m.image.runtime(s.link_addr)), s.mem_size, s.link_addr,
                     true});
            }
        }
        if (m.image.tls) {
            const std::uint64_t index[2] = {m.id, 0};
            add({"tls " + m.file, static_cast<std::byte*>(hle::kernel::tls_block_address(index)),
                 m.image.tls->mem_size, 0, false});
        }
    }
    const auto eboot_tls = hle::kernel::eboot_tls_placement();
    if (!eboot_tls) {
        throw std::logic_error("harness: the test binary doesn't link bbeboottls");
    }
    add({"tls eboot.bin", bytes_at(eboot_tls->block), hle::kernel::eboot_tls_size, 0, false});
    add({"guest errno", reinterpret_cast<std::byte*>(hle::kernel::guest_errno_location()),
         sizeof(int), 0, false});
    // The game's heap and whatever else the init mapped. Only writable mappings can change.
    // Direct memory is a view of the sparse memfd, so only the pages a test touches cost.
    // LeakSanitizer scans the TLS blocks and the errno cell itself, but not these mappings.
    for (const hle::kernel::vmem::Region& r : hle::kernel::vmem::regions()) {
        if ((r.prot & (PROT_READ | PROT_WRITE)) != (PROT_READ | PROT_WRITE)) {
            continue;
        }
        scan_for_leaks(r.start, r.end - r.start);
        Region region{std::format("mapping {:#x}{}{}", r.start, r.name.empty() ? "" : " ", r.name),
                      bytes_at(r.start), r.end - r.start, 0, false};
        if ((r.flags & hle::kernel::vmem::region_direct) != 0) {
            region.fd = hle::kernel::dmem::fd();
            region.fd_offset = r.offset;
        }
        add(std::move(region));
    }
    return regions;
}

// The guest mappings and the direct memory allocations (docs/design/runtime.md, "Guest
// memory"): the restore unmaps what a test mapped and takes back its allocations, so the next
// call gets the same addresses and offsets; the changes are the mappings made and the
// allocations made or released since the baseline.
class GuestMemory : public harness::State {
public:
    std::string_view name() const override { return "guest memory"; }

    void restore() override {
        if (const int error = vmem::restore(mappings_)) {
            throw std::runtime_error(
                std::format("harness: guest mappings not restored: {}", std::strerror(error)));
        }
        if (const int error = hle::kernel::dmem::restore_allocations(allocations_)) {
            throw std::runtime_error(
                std::format("harness: direct memory not restored: {}", std::strerror(error)));
        }
    }

    std::vector<std::string> changes() const override {
        std::vector<std::string> out;
        const std::vector<vmem::Region> table = vmem::regions();
        auto mappings = [&](const std::vector<vmem::Region>& from,
                            const std::vector<vmem::Region>& other, std::string_view what) {
            for (const vmem::Region& r : from) {
                if (std::ranges::find(other, r) == other.end()) {
                    out.push_back(std::format("mapping {:#x}..{:#x} prot {:#x} flags {:#x} offset "
                                              "{:#x}{}{}",
                                              r.start, r.end, r.prot, r.flags, r.offset,
                                              r.reserved ? " reserved" : "", what));
                }
            }
        };
        mappings(mappings_, table, " removed");  // or changed: then also listed as made
        mappings(table, mappings_, "");
        const std::vector<Allocated> now = hle::kernel::dmem::allocations();
        auto list = [&](const std::vector<Allocated>& from, const std::vector<Allocated>& other,
                        std::string_view what) {
            for (const Allocated& a : from) {
                if (std::ranges::find(other, a) == other.end()) {
                    out.push_back(std::format("direct memory {} {:#x}..{:#x} type {}", what,
                                              a.start, a.end, a.type));
                }
            }
        };
        list(now, allocations_, "allocated");
        list(allocations_, now, "released");
        return out;
    }

    bool intact() const override {
        const std::vector<vmem::Region> now = vmem::regions();
        return std::ranges::all_of(
            mappings_, [&](const vmem::Region& r) { return std::ranges::find(now, r) != now.end(); });
    }

private:
    using Allocated = hle::kernel::dmem::AllocatedRange;
    std::vector<vmem::Region> mappings_ = vmem::regions();
    std::vector<Allocated> allocations_ = hle::kernel::dmem::allocations();
};

// The load set and its harness, from the first test that needs them until the process exits.
struct Loaded {
    loader::LoadSet set;
    std::optional<GuestMemory> memory;
    std::optional<harness::Harness> harness;
    std::thread::id thread = std::this_thread::get_id();
};

Loaded* load(const std::filesystem::path& game) {
    auto* l = new Loaded{loader::map_load_set(game), std::nullopt, std::nullopt};
    loader::Image& eboot = hle::prepare(l->set);
    // Game memory holds pointers to runtime objects, such as the host mutexes behind libc's.
    for (const loader::LoadedModule& m : l->set.modules) {
        for (const loader::Segment& s : m.image.segments) {
            if ((s.prot & PROT_WRITE) != 0) {
                scan_for_leaks(m.image.runtime(s.link_addr), s.mem_size);
            }
        }
    }
    game::set_image(&eboot);
    hle::run_prx_init(l->set);
    // The game's allocator maps its heap on the first allocation and keeps it.
    auto* malloc = libc_export<void*(std::size_t)>(l->set, malloc_nid);
    auto* free = libc_export<void(void*)>(l->set, free_nid);
    free(malloc(1));
    l->memory.emplace();
    l->harness.emplace(eboot, load_set_regions(l->set), &*l->memory);
    return l;
}

}  // namespace

void LoadSetTest::SetUp() {
    BB_REQUIRE_GAME(game);
    static Loaded* loaded = load(game);
    ASSERT_EQ(std::this_thread::get_id(), loaded->thread)
        << "the harness's baseline holds the TLS of another thread";
    set_ = &loaded->set;
    harness_ = &*loaded->harness;
    harness_->restore();
}

}  // namespace bb::test
