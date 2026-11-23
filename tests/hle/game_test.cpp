// The HLE's test against the eboot, in eboot_game_test (tests/CMakeLists.txt): it maps the ELF
// that support/eboot_file.hpp read once per process.

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "hle/hle.hpp"
#include "loader/image.hpp"
#include "loader/relocate.hpp"
#include "support/eboot_facts.hpp"
#include "support/eboot_file.hpp"
#include "support/game.hpp"

namespace {

using namespace bb::hle;
namespace facts = bb::test::eboot_facts;

struct Call {
    const StubInfo* stub;
    std::uint64_t calls;
    std::vector<std::uint64_t> args;
};

// Records the calls and installs itself for its lifetime.
struct Recorder : StubObserver {
    std::vector<Call> calls;
    Recorder() { set_stub_observer(this); }
    ~Recorder() override { set_stub_observer(nullptr); }
    void on_call(const StubInfo& stub, const std::uint64_t args[6]) override {
        calls.push_back({&stub, stub.calls, {args, args + 6}});
    }
};

// Every import of the eboot gets a stub or a data block, and a call through a GOT slot reaches
// the stub of the slot's import. The referenced imports get one each; no other test of the binary
// resolves through the HLE.
TEST(HleGame, RelocatesTheEbootWithStubs) {
    BB_REQUIRE_GAME(game);
    const Counts before = counts();
    bb::loader::Image image =
        bb::loader::map_elf(bb::test::eboot_file(game).elf, bb::loader::default_base);
    bb::loader::relocate(image, resolve, bb::loader::eboot_module_id);
    image.finalize_protection();
    // At most 639 of the referenced function imports get a stub: the HLE modules implement the
    // others. A bound rather than the exact number, so that each new HLE function doesn't edit
    // this line.
    static_assert(639 <= facts::referenced_function_imports);
    EXPECT_LE(counts().stubs - before.stubs, 639U);
    EXPECT_EQ(counts().data_blocks - before.data_blocks, facts::object_imports);

    const bb::loader::Rela& rela = image.dyn.plt_relocs.at(0);
    ASSERT_EQ(rela.offset, 0x53e4690U);
    const bb::loader::Import& imp = image.dyn.symbols.at(rela.sym).decoded;
    ASSERT_EQ(imp.nid, "bzQExy189ZI");
    const auto slot = *reinterpret_cast<const std::uintptr_t*>(image.runtime(rela.offset));
    EXPECT_EQ(slot, import_address(imp.library, imp.nid));

    Recorder recorder;
    const auto stub = reinterpret_cast<std::uint64_t (*)(std::uint64_t, std::uint64_t,
                                                         std::uint64_t, std::uint64_t,
                                                         std::uint64_t, std::uint64_t)>(slot);
    EXPECT_EQ(stub(6, 5, 4, 3, 2, 1), 0U);
    ASSERT_EQ(recorder.calls.size(), 1U);
    EXPECT_EQ(recorder.calls[0].stub->nid, "bzQExy189ZI");
    EXPECT_EQ(recorder.calls[0].stub->library, imp.library);
    EXPECT_EQ(recorder.calls[0].args, (std::vector<std::uint64_t>{6, 5, 4, 3, 2, 1}));
}

}  // namespace
