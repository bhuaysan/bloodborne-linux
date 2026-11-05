#include "hle/hle.hpp"

#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
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

void fill_data(std::byte* block) {
    block[0] = std::byte{0x5a};
    block[data_block_size - 1] = std::byte{0xa5};
}
BB_HLE_DATA("libTest", "dataInitA", fill_data);

TEST(Hle, RegisteredDataInitFillsTheBlock) {
    const auto* block = reinterpret_cast<const std::byte*>(resolve(object("dataInitA")));
    EXPECT_EQ(block[0], std::byte{0x5a});
    EXPECT_EQ(block[1], std::byte{0});
    EXPECT_EQ(block[data_block_size - 1], std::byte{0xa5});
    EXPECT_EQ(binding("libTest", "dataInitA"), Binding::data_block);
    EXPECT_EQ(data_blocks().back().nid, "dataInitA");
    EXPECT_THROW(register_data("libTest", "dataInitA", fill_data), std::logic_error);
    resolve(object("dataInitB"));
    EXPECT_THROW(register_data("libTest", "dataInitB", fill_data), std::logic_error);
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

// An export that shadows an implementation is reported through the logger, on hle.<library>,
// in both registration orders, and the filter can silence it.
TEST(Hle, ShadowedImplementationGoesThroughTheLogger) {
    bb::common::set_thread_name("shadow-test");
    testing::internal::CaptureStderr();
    register_function("libShadow", "implFirst", nullptr);
    register_export("libShadow", "implFirst", 0x1000);
    register_export("libShadow", "exportFirst", 0x2000);
    register_function("libShadow", "exportFirst", nullptr);
    bb::common::set_log_filter(bb::common::LogFilter::parse("hle.libShadow=error"));
    register_function("libShadow", "silenced", nullptr);
    register_export("libShadow", "silenced", 0x3000);
    bb::common::set_log_filter(bb::common::LogFilter{});
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_TRUE(std::regex_match(
        log, std::regex("[0-9:.]{12} warn \\[shadow-test\\] hle\\.libShadow: an export of "
                        "libShadow implFirst shadows its implementation\n"
                        "[0-9:.]{12} warn \\[shadow-test\\] hle\\.libShadow: an export of "
                        "libShadow exportFirst shadows its implementation\n")))
        << log;
}

// Sets the log filter for its lifetime, then restores the default.
struct FilterScope {
    explicit FilterScope(std::string_view spec) {
        bb::common::set_log_filter(bb::common::LogFilter::parse(spec));
    }
    ~FilterScope() { bb::common::set_log_filter(bb::common::LogFilter{}); }
    FilterScope(const FilterScope&) = delete;
    FilterScope& operator=(const FilterScope&) = delete;
};

// What the traced implementations below last received.
struct Seen {
    std::array<std::uint64_t, 7> ints;
    std::array<double, 9> reals;
    std::uint64_t returns_to;
};
Seen seen;

// Integer arguments in all six registers and one on the stack; the return value fills rax.
std::uint64_t traced_ints(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d,
                          std::uint64_t e, std::uint64_t f, std::uint64_t g) {
    seen.ints = {a, b, c, d, e, f, g};
    seen.returns_to = reinterpret_cast<std::uint64_t>(__builtin_return_address(0));
    return 0xfedcba9876543210;
}
BB_HLE_FUNCTION("libTraceTest", "tracedInts", traced_ints);

// Floating-point arguments in xmm0-xmm7 and one on the stack; the return value is in xmm0.
double traced_reals(double x0, double x1, double x2, double x3, double x4, double x5, double x6,
                    double x7, double x8, std::uint64_t a) {
    seen.reals = {x0, x1, x2, x3, x4, x5, x6, x7, x8};
    seen.ints[0] = a;
    seen.returns_to = reinterpret_cast<std::uint64_t>(__builtin_return_address(0));
    return -1.25e300;
}
BB_HLE_FUNCTION("libTraceTest", "tracedReals", traced_reals);

// A variadic call passes the number of vector registers in al.
int traced_format(char* out, std::size_t size, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(out, size, format, args);
    va_end(args);
    return n;
}
BB_HLE_FUNCTION("libTraceTest", "tracedFormt", traced_format);

// Returns rax as it was on entry.
extern "C" std::uint64_t bb_test_echo_rax();
asm(".text\n"
    ".type bb_test_echo_rax, @function\n"
    "bb_test_echo_rax:\n"
    "    ret\n"
    ".size bb_test_echo_rax, . - bb_test_echo_rax\n");
BB_HLE_FUNCTION("libTraceTest", "tracedRax", bb_test_echo_rax);

// Calls `fn` with rax = `value` and returns rax after the call.
std::uint64_t call_with_rax(std::uintptr_t fn, std::uint64_t value) {
    std::uint64_t out;
    asm volatile(
        "mov %%rsp, %%rbx\n\t"
        "sub $128, %%rsp\n\t"  // past the red zone
        "and $-16, %%rsp\n\t"
        "call *%[fn]\n\t"
        "mov %%rbx, %%rsp"
        : "=a"(out)
        : "a"(value), [fn] "r"(fn)
        : "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "xmm0", "xmm1", "xmm2",
          "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12",
          "xmm13", "xmm14", "xmm15", "memory", "cc");
    return out;
}

Import trace_import(std::string nid) {
    return {std::move(nid), "libTraceTest", "libTraceTestModule", bb::loader::stt_func};
}

// Matches one trace line on hle.libTraceTest whose message starts with `start`.
[[maybe_unused]] bool is_trace_line(const std::string& log, const std::string& start) {
    static const std::regex line("[0-9:.]{12} trace \\[trace-test\\] hle\\.libTraceTest: [^\n]*\n");
    const std::string prefix = "] hle.libTraceTest: " + start;
    return std::regex_match(log, line) && log.find(prefix) != std::string::npos;
}

// With trace shown for hle.<library> when it is resolved, a call through the GOT slot of an
// implementation logs one line with the address it returns to, and the implementation gets
// every argument and returns its value unchanged.
TEST(HleTrace, LogsImplementationCallsThroughTheGot) {
    std::array<std::uintptr_t, 2> got{};
    {
        const FilterScope trace("hle.libTraceTest=trace");
        got = {resolve(trace_import("tracedInts")), resolve(trace_import("tracedReals"))};
    }
#ifdef NDEBUG
    EXPECT_EQ(got[0], reinterpret_cast<std::uintptr_t>(&traced_ints));
    GTEST_SKIP() << "HLE calls are traced in builds without NDEBUG only";
#else
    EXPECT_NE(got[0], reinterpret_cast<std::uintptr_t>(&traced_ints));
    EXPECT_NE(got[1], reinterpret_cast<std::uintptr_t>(&traced_reals));
    EXPECT_EQ(resolve(trace_import("tracedInts")), got[0]);
    EXPECT_EQ(import_address("libTraceTest", "tracedInts"), got[0]);
    EXPECT_EQ(binding("libTraceTest", "tracedInts"), Binding::implemented);
    bb::common::set_thread_name("trace-test");
    const FilterScope trace("hle.libTraceTest=trace");

    testing::internal::CaptureStderr();
    const auto ints = reinterpret_cast<decltype(&traced_ints)>(std::as_const(got)[0]);
    EXPECT_EQ(ints(1, 0x22, 0x333, 0x4444, 0x55555, 0xfedcba9876543210, 7), 0xfedcba9876543210);
    std::string log = testing::internal::GetCapturedStderr();
    EXPECT_EQ(seen.ints, (std::array<std::uint64_t, 7>{1, 0x22, 0x333, 0x4444, 0x55555,
                                                       0xfedcba9876543210, 7}));
    EXPECT_TRUE(is_trace_line(
        log, std::format("call libTraceTest (libTraceTestModule) tracedInts ? from {:#x}: 0x1 "
                         "0x22 0x333 0x4444 0x55555 0xfedcba9876543210\n",
                         seen.returns_to)))
        << log;

    testing::internal::CaptureStderr();
    const auto reals = reinterpret_cast<decltype(&traced_reals)>(std::as_const(got)[1]);
    EXPECT_EQ(reals(0.5, -1.0, 2.25, 3e10, -4e-10, 5.0, 6.125, 7.0, 8.5, 0x99), -1.25e300);
    log = testing::internal::GetCapturedStderr();
    EXPECT_EQ(seen.reals, (std::array<double, 9>{0.5, -1.0, 2.25, 3e10, -4e-10, 5.0, 6.125, 7.0,
                                                 8.5}));
    EXPECT_EQ(seen.ints[0], 0x99U);
    EXPECT_TRUE(is_trace_line(
        log, std::format("call libTraceTest (libTraceTestModule) tracedReals ? from {:#x}: 0x99 ",
                         seen.returns_to)))
        << log;
#endif
}

// The thunk keeps rax, which a variadic call sets to the number of vector registers it uses.
TEST(HleTrace, KeepsRax) {
    std::uintptr_t format_addr = 0;
    std::uintptr_t rax_addr = 0;
    {
        const FilterScope trace("hle.libTraceTest=trace");
        format_addr = resolve(trace_import("tracedFormt"));
        rax_addr = resolve(trace_import("tracedRax"));
    }
    const FilterScope trace("hle.libTraceTest=trace");
    testing::internal::CaptureStderr();
    char text[64];
    const int n = reinterpret_cast<decltype(&traced_format)>(format_addr)(
        text, sizeof text, "%d %.2f %s %.1f", 17, 2.25, "x", -0.5);
    const std::uint64_t rax = call_with_rax(rax_addr, 0x0123456789abcdef);
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_EQ(n, 14);
    EXPECT_STREQ(text, "17 2.25 x -0.5");
    EXPECT_EQ(rax, 0x0123456789abcdefU);
#ifndef NDEBUG
    EXPECT_NE(log.find("call libTraceTest (libTraceTestModule) tracedFormt ? from 0x"),
              std::string::npos)
        << log;
    EXPECT_NE(log.find("call libTraceTest (libTraceTestModule) tracedRax ? from 0x"),
              std::string::npos)
        << log;
#endif
}

std::uint64_t untraced(std::uint64_t x) {
    return x + 1;
}
BB_HLE_FUNCTION("libTraceOff", "untraced", untraced);
BB_HLE_FUNCTION("libTraceTest", "filterLater", untraced);

// Without trace for hle.<library> at resolve time, the GOT gets the implementation itself, and
// once traced, the filter at call time decides whether a line is written. Exports and stubs are
// never traced.
TEST(HleTrace, OnlyWhenTheFilterShowsTraceAtResolve) {
    std::uintptr_t later = 0;
    {
        const FilterScope trace("hle=trace,hle.libTraceOff=debug");
        EXPECT_EQ(resolve({"untraced", "libTraceOff", "libTraceOffModule", bb::loader::stt_func}),
                  reinterpret_cast<std::uintptr_t>(&untraced));
        register_export("libTraceTest", "tracedExprt", 0x1234);
        EXPECT_EQ(resolve(trace_import("tracedExprt")), 0x1234U);
        resolve(trace_import("tracedStub1"));
        EXPECT_EQ(binding("libTraceTest", "tracedStub1"), Binding::stub);
        later = resolve(trace_import("filterLater"));
    }
    testing::internal::CaptureStderr();
    EXPECT_EQ(reinterpret_cast<decltype(&untraced)>(later)(41), 42U);
    EXPECT_EQ(testing::internal::GetCapturedStderr(), "");
}

// Every import of the eboot gets a stub or a data block, and a call through a GOT slot reaches
// the stub of the slot's import. 686 of the 701 imports are referenced, 20 of them data.
TEST(HleGame, RelocatesTheEbootWithStubs) {
    BB_REQUIRE_GAME(game);
    const Counts before = counts();
    bb::loader::Image image = bb::loader::map_eboot(game);
    bb::loader::relocate(image, resolve, bb::loader::eboot_module_id);
    image.finalize_protection();
    // The HLE modules implement some of the function imports, so those get no stub.
    EXPECT_EQ(counts().stubs - before.stubs, 645U);
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
