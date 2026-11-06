// The PRX initialisers of src/hle/prx_init.cpp: a fake initialiser without the game, and the load
// set's on a thread set up like the game's main thread, followed by calls of libc's exports.

#include <pthread.h>
#include <sys/mman.h>

#ifdef __SANITIZE_ADDRESS__
#include <sanitizer/lsan_interface.h>
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "hle/hle.hpp"
#include "hle/kernel/tls.hpp"
#include "hle/kernel/vmem.hpp"
#include "hle/link.hpp"
#include "hle/prx_init.hpp"
#include "loader/load_set.hpp"
#include "support/game.hpp"

namespace {

using bb::hle::PrxNext;

// ---- without the game --------------------------------------------------------------------------

struct InitCall {
    std::uint64_t arg = 1;
    const void* argp = &arg;
    PrxNext* next = reinterpret_cast<PrxNext*>(1);
    int calls = 0;
};
InitCall fake_call;

std::int32_t fake_init(std::uint64_t arg, const void* argp, PrxNext* next) {
    fake_call = {arg, argp, next, fake_call.calls + 1};
    return 0;
}

std::int32_t failing_init(std::uint64_t, const void*, PrxNext*) {
    return 0x8002000c;  // what a module_start might return
}

TEST(PrxInit, CallsTheInitialiserWithNoArguments) {
    bb::hle::run_prx_initialiser("fake.prx", &fake_init);
    EXPECT_EQ(fake_call.calls, 1);
    EXPECT_EQ(fake_call.arg, 0U);
    EXPECT_EQ(fake_call.argp, nullptr);
    EXPECT_EQ(fake_call.next, nullptr);
}

TEST(PrxInitDeathTest, AbortsOnANonZeroResult) {
    EXPECT_DEATH(bb::hle::run_prx_initialiser("fake.prx", &failing_init),
                 "fake.prx: initialiser failed with 0x8002000c");
}

TEST(PrxInit, RefusesALoadSetWithoutProcessParameters) {
    const bb::loader::LoadSet empty;
    EXPECT_THROW(bb::hle::run_prx_init(empty), std::invalid_argument);
}

// ---- the game ----------------------------------------------------------------------------------

// Runs `fn` on a thread set up like the game's main thread (docs/design/m3-core-os.md, Entry): a
// pthread with a 16 MiB stack, whose eboot TLS block (this test links bbeboottls) ends at its
// thread pointer.
void on_game_thread(const std::function<void()>& fn) {
    pthread_attr_t attr;
    ASSERT_EQ(pthread_attr_init(&attr), 0);
    ASSERT_EQ(pthread_attr_setstacksize(&attr, std::size_t{16} << 20), 0);
    struct Call {
        const std::function<void()>* fn;
        std::exception_ptr error;
    } call{&fn, nullptr};
    pthread_t thread;
    const auto start = [](void* p) -> void* {
        auto* c = static_cast<Call*>(p);
        bb::hle::kernel::check_eboot_tls();
        try {
            (*c->fn)();
        } catch (...) {
            c->error = std::current_exception();
        }
        return nullptr;
    };
    ASSERT_EQ(pthread_create(&thread, &attr, start, &call), 0);
    ASSERT_EQ(pthread_join(thread, nullptr), 0);
    pthread_attr_destroy(&attr);
    if (call.error) {
        std::rethrow_exception(call.error);
    }
}

// Records the stubs that game code calls instead of logging them.
class StubRecorder : public bb::hle::StubObserver {
public:
    void on_call(const bb::hle::StubInfo& stub, const std::uint64_t[6]) override {
        const std::lock_guard lock(mutex_);
        ++calls_[stub.library + " " + stub.nid];
    }
    std::map<std::string, int> calls() {
        const std::lock_guard lock(mutex_);
        return calls_;
    }

private:
    std::mutex mutex_;
    std::map<std::string, int> calls_;
};

// Game memory holds pointers to runtime objects (e.g. the host mutex behind a guest mutex), and
// LeakSanitizer doesn't scan mappings of our own. Registers the mappings that can hold them.
void scan_for_leaks(std::uintptr_t start, std::size_t size) {
#ifdef __SANITIZE_ADDRESS__
    __lsan_register_root_region(reinterpret_cast<const void*>(start), size);
#else
    static_cast<void>(start);
    static_cast<void>(size);
#endif
}

// The load set, linked and initialised once per process: the HLE registry takes one load set.
// Never destroyed, so the game's memory stays mapped until LeakSanitizer has scanned it.
struct Initialised {
    bb::loader::LoadSet set;
    std::map<std::string, int> init_stubs;  // "library NID" -> calls during the init
    std::string init_log;                   // stderr during the init
};

const Initialised& initialised(const std::filesystem::path& game) {
    static Initialised* state = nullptr;
    static std::once_flag once;
    std::call_once(once, [&] {
        auto* s = new Initialised{bb::loader::map_load_set(game), {}, {}};
        bb::hle::link(s->set);
        for (bb::loader::LoadedModule& m : s->set.modules) {
            m.image.finalize_protection();
            for (const bb::loader::Segment& seg : m.image.segments) {
                if ((seg.prot & PROT_WRITE) != 0) {
                    scan_for_leaks(m.image.runtime(seg.link_addr), seg.mem_size);
                }
            }
        }
        StubRecorder recorder;
        bb::hle::set_stub_observer(&recorder);
        testing::internal::CaptureStderr();
        on_game_thread([&] { bb::hle::run_prx_init(s->set); });
        s->init_log = testing::internal::GetCapturedStderr();
        bb::hle::set_stub_observer(nullptr);
        s->init_stubs = recorder.calls();
        state = s;
    });
    return *state;
}

const bb::loader::Image& module(const Initialised& s, std::size_t index) {
    return s.set.modules.at(index).image;
}

// The address of the libc.prx export `nid` (library libc).
template <class F>
F* libc_export(const Initialised& s, std::string_view nid) {
    for (const bb::loader::Export& e : bb::loader::exports(module(s, 2))) {
        if (e.library == "libc" && e.nid == nid) {
            return reinterpret_cast<F*>(e.address);
        }
    }
    throw std::runtime_error("libc.prx exports no " + std::string(nid));
}

// The imports the init may reach as stubs, with the reason. Addresses are libc.prx's. An import
// may leave this list once it is implemented; the test only checks that no other stub is called.
const std::map<std::string, std::string_view> expected_init_stubs = {
    // Preinit, through 0x1fb58: hands the allocator callbacks of the eboot's tables to the kernel
    // side (docs/formats/kernel-imports.md). Its result is ignored.
    {"libkernel p5EcQeEeJAE", "allocator callbacks, result ignored"},
    // Constructor 0x5fad0 jumps to 0x81a80, which counts the CPUs in the calling thread's
    // affinity mask (attr init, self, attr get, get affinity, attr destroy) into 0xd1c54, what
    // libc's std::thread::hardware_concurrency (0x5fac0) returns. No module of the load set
    // imports that. The second threads PR of #94 implements the three that are still stubs;
    // until then the mask and the count stay 0.
    {"libkernel aI+OeCz8xrQ", "scePthreadSelf, CPU count (#94)"},
    {"libkernel x1X76arYMxU", "scePthreadAttrGet, CPU count (#94)"},
    {"libkernel 8+s5BzZjxSg", "scePthreadAttrGetaffinity, CPU count (#94)"},
};

// Both initialisers return 0 (run_prx_initialiser aborts otherwise). libc's preinit accepts the
// process parameters: a failed check reports 0xa0020010 or 0xa0020013 through OMDRKKAZ8I4, which
// logs a status report and aborts.
TEST(PrxInitGame, RunsBothInitialisersWithOnlyExpectedStubs) {
    BB_REQUIRE_GAME(game);
    const Initialised& s = initialised(game);
    EXPECT_NE(s.init_log.find("libc.prx: initialised"), std::string::npos) << s.init_log;
    EXPECT_EQ(s.init_log.find("status report"), std::string::npos) << s.init_log;
    for (const auto& [stub, calls] : s.init_stubs) {
        EXPECT_TRUE(expected_init_stubs.contains(stub)) << stub << " called " << calls << " times";
    }
    EXPECT_THROW(bb::hle::run_prx_init(s.set), std::logic_error);
}

// libc's preinit copied the eboot's first allocator table (block +0x30) into its dispatch cells:
// malloc (0x1f850) jumps through 0xba190, which holds the table's +0x20, the eboot's 0x20809f0.
// The game's allocator maps its heap as direct memory on the first allocation.
TEST(PrxInitGame, MallocReachesTheGamesAllocator) {
    BB_REQUIRE_GAME(game);
    const Initialised& s = initialised(game);
    std::uintptr_t cell = 0;
    std::memcpy(&cell, reinterpret_cast<const void*>(module(s, 2).runtime(0xba190)), sizeof cell);
    EXPECT_EQ(cell, module(s, 0).runtime(0x20809f0));

    auto* malloc = libc_export<void*(std::size_t)>(s, "gQX+4GDQjpM");
    auto* free = libc_export<void(void*)>(s, "tIhsqj0qsFE");
    on_game_thread([&] {
        void* p = malloc(100);
        ASSERT_NE(p, nullptr);
        std::memset(p, 0x5a, 100);
        const std::optional<bb::hle::kernel::vmem::Region> region =
            bb::hle::kernel::vmem::find(reinterpret_cast<std::uintptr_t>(p));
        ASSERT_TRUE(region.has_value());
        EXPECT_NE(region->flags & bb::hle::kernel::vmem::region_direct, 0U);
        scan_for_leaks(region->start, region->end - region->start);
        free(p);
    });
}

TEST(PrxInitGame, SnprintfFormats) {
    BB_REQUIRE_GAME(game);
    const Initialised& s = initialised(game);
    auto* snprintf = libc_export<int(char*, std::size_t, const char*, ...)>(s, "eLdDw6l0-bU");
    on_game_thread([&] {
        char buffer[32];
        std::memset(buffer, 'x', sizeof buffer);
        EXPECT_EQ(snprintf(buffer, sizeof buffer, "%s %d %#x %.2f", "abc", -42, 255, 1.5), 17);
        EXPECT_STREQ(buffer, "abc -42 0xff 1.50");
        EXPECT_EQ(snprintf(buffer, 4, "%d", 123456), 6);  // cut, with the length it wanted
        EXPECT_STREQ(buffer, "123");
    });
}

}  // namespace
