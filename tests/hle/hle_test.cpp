#include "hle/hle.hpp"

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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
    // The M2 text, as a warning on the channel hle.<library>.
    EXPECT_NE(log.find(" warn ["), std::string::npos) << log;
    EXPECT_NE(log.find("] hle.libTest: stub libTest (libTestModule) loggedStub1 ? call 1: 0xabc "),
              std::string::npos)
        << log;
}

// Integer arguments in all six registers and on the stack, and floating-point ones.
double traced_sum(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d,
                  std::uint64_t e, std::uint64_t f, std::uint64_t g, double x, double y) {
    return static_cast<double>(a + b + c + d + e + f + g) + x * y;
}
BB_HLE_FUNCTION("libTraceTest", "tracedSum", traced_sum);

// A variadic call passes the number of vector registers in al.
int traced_format(char* out, std::size_t size, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(out, size, format, args);
    va_end(args);
    return n;
}
BB_HLE_FUNCTION("libTraceTest", "tracedFormat", traced_format);

// With trace level for its channel at resolve time, an implementation call is logged and still
// gets every argument.
TEST(Hle, TracesImplementationCallsInDebugBuilds) {
#ifdef NDEBUG
    GTEST_SKIP() << "HLE call tracing exists in debug builds only";
#else
    bb::common::set_log_filter(bb::common::LogFilter::parse("hle.libTraceTest=trace"));
    const Import sum_imp{"tracedSum", "libTraceTest", "libTraceTestModule", bb::loader::stt_func};
    const Import format_imp{"tracedFormat", "libTraceTest", "libTraceTestModule",
                            bb::loader::stt_func};
    const std::uintptr_t sum_addr = resolve(sum_imp);
    const std::uintptr_t format_addr = resolve(format_imp);
    bb::common::set_log_filter({});
    EXPECT_NE(sum_addr, reinterpret_cast<std::uintptr_t>(&traced_sum));
    EXPECT_EQ(resolve(sum_imp), sum_addr);
    EXPECT_EQ(import_address("libTraceTest", "tracedSum"), sum_addr);

    // The filter at call time decides whether the line is written.
    bb::common::set_log_filter(bb::common::LogFilter::parse("hle=trace"));
    testing::internal::CaptureStderr();
    const double sum = reinterpret_cast<decltype(&traced_sum)>(sum_addr)(1, 2, 3, 4, 5, 6, 7, 1.5,
                                                                         4.0);
    char text[64];
    const int n = reinterpret_cast<decltype(&traced_format)>(format_addr)(
        text, sizeof text, "%d %.2f %s", 17, 2.25, "x");
    const std::string log = testing::internal::GetCapturedStderr();
    bb::common::set_log_filter({});

    EXPECT_EQ(sum, 34.0);
    EXPECT_EQ(n, 9);
    EXPECT_STREQ(text, "17 2.25 x");
    EXPECT_NE(log.find(" trace ["), std::string::npos) << log;
    EXPECT_NE(log.find("] hle.libTraceTest: call libTraceTest tracedSum ?: 0x1 0x2 0x3 0x4 0x5 "
                       "0x6\n"),
              std::string::npos)
        << log;
    EXPECT_NE(log.find("call libTraceTest tracedFormat ?: "), std::string::npos) << log;
#endif
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
