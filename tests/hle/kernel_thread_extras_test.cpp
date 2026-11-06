// Thread names, priority and affinity, called through their registered NIDs. This test links
// bbeboottls, so new threads run the eboot TLS check.

#include <pthread.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "common/log.hpp"
#include "hle/hle.hpp"
#include "hle/kernel/thread.hpp"
#include "loader/dynamic.hpp"
#include "support/hle.hpp"

namespace {

using namespace bb::hle::kernel;
using Cell = std::uintptr_t;
using Handle = Thread*;

using bb::test::fn;

auto* const create =
    fn<std::int32_t(Handle*, const Cell*, ThreadEntry, void*, const char*)>("6UgtwV+0zb4");
auto* const join = fn<std::int32_t(Handle, void**)>("onNY9Byn-W8");
auto* const self = fn<Handle()>("aI+OeCz8xrQ");
auto* const attr_init = fn<std::int32_t(Cell*)>("nsYoNRywwNg");
auto* const attr_destroy = fn<std::int32_t(Cell*)>("62KCwEMmzcM");
auto* const attr_get = fn<std::int32_t(Handle, Cell*)>("x1X76arYMxU");
auto* const attr_setaffinity = fn<std::int32_t(Cell*, std::uint64_t)>("3qxgM4ezETA");
auto* const attr_getaffinity = fn<std::int32_t(Cell*, std::uint64_t*)>("8+s5BzZjxSg");
auto* const rename = fn<std::int32_t(Handle, const char*)>("GBUY7ywdULE");
auto* const setaffinity = fn<std::int32_t(Handle, std::uint64_t)>("bt3CTBKmGyI");
auto* const getaffinity = fn<std::int32_t(Handle, std::uint64_t*)>("rcrVFJsQWRY");
auto* const set_priority = fn<std::int32_t(Handle, int)>("W0Hpm2X0uPE");
auto* const get_priority = fn<std::int32_t(Handle, int*)>("1tKyG7RlMJo");

constexpr std::int32_t esrch = 0x80020003, einval = 0x80020016;

Handle start(ThreadEntry entry, void* arg = nullptr, const Cell* attr = nullptr,
             const char* name = nullptr) {
    Handle thread = nullptr;
    EXPECT_EQ(create(&thread, attr, entry, arg, name), 0);
    return thread;
}

// ---- names -----------------------------------------------------------------------------------

std::string host_name() {
    char name[16] = {};
    pthread_getname_np(pthread_self(), name, sizeof name);
    return name;
}

void* rename_self(void*) {
    EXPECT_EQ(rename(self(), "renamed thread with a long name"), 0);
    bb::common::log(bb::common::LogLevel::info, "test", "after the rename");
    EXPECT_EQ(host_name(), "renamed thread ");
    return nullptr;
}

TEST(Rename, TheNewNameShowsInTheLogLineAndForTheHost) {
    testing::internal::CaptureStderr();
    ASSERT_EQ(join(start(rename_self, nullptr, nullptr, "old"), nullptr), 0);
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("thread \"old\" renamed to \"renamed thread with a long name\""),
              std::string::npos)
        << log;
    EXPECT_NE(log.find("[renamed thread with a long name] test: after the rename"),
              std::string::npos)
        << log;
}

std::atomic<bool> may_end{false};
void* wait_to_end(void*) {
    while (!may_end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return reinterpret_cast<void*>(host_name() == "other");
}

TEST(Rename, AnotherThreadGetsTheNameForTheHost) {
    may_end = false;
    Handle thread = start(wait_to_end, nullptr, nullptr, "before");
    EXPECT_EQ(rename(thread, "other"), 0);
    may_end = true;
    void* renamed = nullptr;
    ASSERT_EQ(join(thread, &renamed), 0);
    EXPECT_NE(renamed, nullptr);
}

TEST(Rename, InvalidArgumentsAreRefused) {
    EXPECT_EQ(rename(nullptr, "x"), esrch);
    EXPECT_EQ(rename(self(), nullptr), einval);
}

// ---- priority and affinity -------------------------------------------------------------------

TEST(Affinity, TheAttributeRoundTripsAndDefaultsToSixCores) {
    Cell attr = 0;
    ASSERT_EQ(attr_init(&attr), 0);
    std::uint64_t mask = 0;
    ASSERT_EQ(attr_getaffinity(&attr, &mask), 0);
    EXPECT_EQ(mask, 0x3f);
    ASSERT_EQ(attr_setaffinity(&attr, 0x21), 0);
    ASSERT_EQ(attr_getaffinity(&attr, &mask), 0);
    EXPECT_EQ(mask, 0x21);
    EXPECT_EQ(attr_getaffinity(&attr, nullptr), einval);
    EXPECT_EQ(attr_destroy(&attr), 0);
    EXPECT_EQ(attr_setaffinity(&attr, 1), einval);
    EXPECT_EQ(attr_getaffinity(&attr, &mask), einval);
}

void* report_affinity(void*) {
    std::uint64_t mask = 0;
    EXPECT_EQ(getaffinity(self(), &mask), 0);
    return reinterpret_cast<void*>(mask);
}

TEST(Affinity, ANewThreadGetsTheAttributesMask) {
    Cell attr = 0;
    ASSERT_EQ(attr_init(&attr), 0);
    ASSERT_EQ(attr_setaffinity(&attr, 0x30), 0);
    void* mask = nullptr;
    ASSERT_EQ(join(start(report_affinity, nullptr, &attr), &mask), 0);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(mask), 0x30u);
    EXPECT_EQ(attr_destroy(&attr), 0);
}

TEST(Affinity, SetAndGetRoundTripAndShowInTheAttributes) {
    Handle me = self();
    std::uint64_t mask = 0;
    ASSERT_EQ(getaffinity(me, &mask), 0);
    EXPECT_EQ(mask, 0x3f);
    ASSERT_EQ(setaffinity(me, 0xe), 0);
    ASSERT_EQ(getaffinity(me, &mask), 0);
    EXPECT_EQ(mask, 0xe);
    // libc counts the CPUs from the mask of the thread's attributes (0x81a90).
    Cell attr = 0;
    ASSERT_EQ(attr_init(&attr), 0);
    ASSERT_EQ(attr_get(me, &attr), 0);
    ASSERT_EQ(attr_getaffinity(&attr, &mask), 0);
    EXPECT_EQ(mask, 0xe);
    EXPECT_EQ(attr_destroy(&attr), 0);
    ASSERT_EQ(setaffinity(me, 0x3f), 0);
    EXPECT_EQ(setaffinity(nullptr, 1), esrch);
    EXPECT_EQ(getaffinity(nullptr, &mask), esrch);
    EXPECT_EQ(getaffinity(me, nullptr), einval);
}

TEST(Priority, SetAndGetRoundTrip) {
    Handle me = self();
    ASSERT_EQ(set_priority(me, 0x2fe), 0);
    int priority = 0;
    ASSERT_EQ(get_priority(me, &priority), 0);
    EXPECT_EQ(priority, 0x2fe);
    EXPECT_EQ(set_priority(nullptr, 0x100), esrch);
    EXPECT_EQ(get_priority(nullptr, &priority), esrch);
    EXPECT_EQ(get_priority(me, nullptr), einval);
}

}  // namespace
