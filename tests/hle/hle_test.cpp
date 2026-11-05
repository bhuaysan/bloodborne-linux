#include "hle/hle.hpp"

#include <cstddef>
#include <cstdint>
#include <regex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "common/log.hpp"
#include "loader/image.hpp"
#include "loader/relocate.hpp"
#include "support/game.hpp"

namespace {

using namespace bb::hle;
using bb::loader::Import;

// Each test uses its own NIDs: the registry is global to the process.
Import function(std::string nid) {
    return {std::move(nid), "libTest", "libTestModule", bb::loader::stt_func};
}

Import object(std::string nid) {
    return {std::move(nid), "libTest", "libTestModule", bb::loader::stt_object};
}

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

std::uint64_t answer(std::uint64_t x) {
    return x + 42;
}
BB_HLE_FUNCTION("libTest", "registeredA", answer);

TEST(Hle, StubPassesArgumentsAndReturnsZero) {
    const auto stub = reinterpret_cast<std::uint64_t (*)(std::uint64_t, std::uint64_t,
                                                         std::uint64_t, std::uint64_t,
                                                         std::uint64_t, std::uint64_t)>(
        resolve(function("wZi5ly2guNw")));
    Recorder recorder;
    EXPECT_EQ(stub(1, 0x22, 0x333, 0x4444, 0x55555, 0xfedcba9876543210), 0U);
    EXPECT_EQ(reinterpret_cast<double (*)()>(resolve(function("wZi5ly2guNw")))(), 0.0);

    ASSERT_EQ(recorder.calls.size(), 2U);
    const StubInfo& info = *recorder.calls[0].stub;
    EXPECT_EQ(info.library, "libTest");
    EXPECT_EQ(info.module, "libTestModule");
    EXPECT_EQ(info.nid, "wZi5ly2guNw");
    EXPECT_EQ(info.name, "_Exit");
    EXPECT_EQ(recorder.calls[0].calls, 1U);
    EXPECT_EQ(recorder.calls[0].args,
              (std::vector<std::uint64_t>{1, 0x22, 0x333, 0x4444, 0x55555, 0xfedcba9876543210}));
    EXPECT_EQ(recorder.calls[1].stub, &info);
    EXPECT_EQ(recorder.calls[1].calls, 2U);
}

TEST(Hle, NidNameFromTheTable) {
    EXPECT_EQ(nid_name("wZi5ly2guNw"), "_Exit");  // in symbols/nids.csv
    EXPECT_EQ(nid_name("AAAAAAAAAAA"), "?");
    EXPECT_EQ(nid_name(""), "?");
}

TEST(Hle, ResolveIsIdempotent) {
    const auto before = counts();
    const std::uintptr_t a = resolve(function("idempotentA"));
    const std::uintptr_t b = resolve(function("idempotentB"));
    EXPECT_NE(a, b);
    EXPECT_EQ(resolve(function("idempotentA")), a);
    EXPECT_EQ(import_address("libTest", "idempotentA"), a);
    EXPECT_EQ(counts().stubs, before.stubs + 2);
    EXPECT_EQ(counts().data_blocks, before.data_blocks);
}

TEST(HleDeathTest, ImportAddressAbortsWhenNeverResolved) {
    EXPECT_DEATH(import_address("libTest", "neverResolv"), "never resolved");
}

TEST(Hle, DataBlockIsZeroFilledAndCached) {
    const auto before = counts();
    auto* a = reinterpret_cast<unsigned char*>(resolve(object("dataBlockAA")));
    auto* b = reinterpret_cast<unsigned char*>(resolve(object("dataBlockBB")));
    EXPECT_NE(a, b);
    for (std::size_t i = 0; i < data_block_size; ++i) {
        ASSERT_EQ(a[i], 0) << i;
    }
    a[4095] = 1;  // writable to the end
    EXPECT_EQ(b[0], 0);
    EXPECT_EQ(reinterpret_cast<unsigned char*>(resolve(object("dataBlockAA"))), a);
    EXPECT_EQ(counts().data_blocks, before.data_blocks + 2);
    EXPECT_EQ(counts().stubs, before.stubs);
    const std::vector<DataBlock> blocks = data_blocks();
    ASSERT_EQ(blocks.size(), before.data_blocks + 2);
    const DataBlock& last_a = blocks[blocks.size() - 2];
    EXPECT_EQ(last_a.library, "libTest");
    EXPECT_EQ(last_a.nid, "dataBlockAA");
    EXPECT_EQ(last_a.data, reinterpret_cast<std::byte*>(a));
    EXPECT_EQ(blocks.back().nid, "dataBlockBB");
    EXPECT_EQ(blocks.back().data, reinterpret_cast<std::byte*>(b));
}

TEST(Hle, RegisteredFunctionWinsOverStub) {
    const auto impl = reinterpret_cast<std::uint64_t (*)(std::uint64_t)>(
        resolve(function("registeredA")));
    EXPECT_EQ(impl, &answer);
    EXPECT_EQ(impl(1), 43U);
    EXPECT_THROW(register_function("libTest", "registeredA", nullptr), std::logic_error);
    resolve(function("registeredB"));
    EXPECT_THROW(register_function("libTest", "registeredB", nullptr), std::logic_error);
}

TEST(Hle, LogsFirstTenThenEveryThousandth) {
    for (std::uint64_t call = 1; call <= 10; ++call) {
        EXPECT_TRUE(should_log(call)) << call;
    }
    EXPECT_FALSE(should_log(11));
    EXPECT_FALSE(should_log(999));
    EXPECT_TRUE(should_log(1000));
    EXPECT_FALSE(should_log(1001));
    EXPECT_TRUE(should_log(2000));
}

TEST(Hle, LogsWithoutObserver) {
    const auto stub = reinterpret_cast<void (*)(std::uint64_t)>(resolve(function("loggedStub1")));
    testing::internal::CaptureStderr();
    stub(0xabc);
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("libTest (libTestModule) loggedStub1 ? call 1: 0xabc "), std::string::npos)
        << log;
}

// The stub line goes through the logger, on channel hle.<library>, with #44's message.
TEST(Hle, StubLogGoesThroughTheLogger) {
    using Fn = void (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                        std::uint64_t);
    const auto stub = reinterpret_cast<Fn>(resolve(function("loggedStub2")));
    bb::common::set_thread_name("stub-test");
    testing::internal::CaptureStderr();
    stub(0, 0x10, 2, 3, 4, 0xfedcba9876543210);
    bb::common::set_log_filter(bb::common::LogFilter::parse("hle.libTest=error"));
    stub(0, 0x10, 2, 3, 4, 5);
    bb::common::set_log_filter(bb::common::LogFilter{});
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_TRUE(std::regex_match(
        log, std::regex("[0-9:.]{12} warn \\[stub-test\\] hle\\.libTest: stub libTest "
                        "\\(libTestModule\\) loggedStub2 \\? call 1: 0 0x10 0x2 0x3 0x4 "
                        "0xfedcba9876543210\n")))
        << log;
}

// Every import of the eboot gets a stub or a data block, and a call through a GOT slot reaches
// the stub of the slot's import. 686 of the 701 imports are referenced, 20 of them data.
TEST(HleGame, RelocatesTheEbootWithStubs) {
    BB_REQUIRE_GAME(game);
    const Counts before = counts();
    bb::loader::Image image = bb::loader::map_eboot(game);
    bb::loader::relocate(image, resolve, bb::loader::eboot_module_id);
    image.finalize_protection();
    EXPECT_EQ(counts().stubs - before.stubs, 666U);
    EXPECT_EQ(counts().data_blocks - before.data_blocks, 20U);

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
