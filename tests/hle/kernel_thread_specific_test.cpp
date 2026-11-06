// Thread-specific data and once, called through their registered NIDs. This test links
// bbeboottls, so new threads run the eboot TLS check.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

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
auto* const exit = fn<void(void*)>("3kg7rT0NQIs");
auto* const key_create = fn<std::int32_t(int*, void (*)(void*))>("geDaqgH9lTg");
auto* const key_delete = fn<std::int32_t(int)>("PrdHuuDekhY");
auto* const getspecific = fn<void*(int)>("eoht7mQOCmo");
auto* const setspecific = fn<std::int32_t(int, const void*)>("+BzXYkqYeLE");
auto* const once = fn<std::int32_t(int*, void (*)())>("14bOACANTBo");

constexpr std::int32_t einval = 0x80020016;
constexpr std::int32_t eagain = 0x80020023;

Handle start(ThreadEntry entry, void* arg = nullptr) {
    Handle thread = nullptr;
    EXPECT_EQ(create(&thread, nullptr, entry, arg, nullptr), 0);
    return thread;
}

// ---- keys ------------------------------------------------------------------------------------

int key;  // the key the entries below use
std::vector<std::uintptr_t> destroyed;  // values passed to the destructor, in order
std::atomic<int> rearm{0};  // how often the destructor sets the value again

void record(void* value) {
    destroyed.push_back(reinterpret_cast<std::uintptr_t>(value));
    if (rearm > 0) {
        --rearm;
        setspecific(key, reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(value) + 1));
    }
}

void* set_value(void* value) {
    EXPECT_EQ(getspecific(key), nullptr);
    EXPECT_EQ(setspecific(key, value), 0);
    EXPECT_EQ(getspecific(key), value);
    return nullptr;
}

void* set_value_and_exit(void* value) {
    set_value(value);
    exit(nullptr);
    return nullptr;  // not reached
}

TEST(ThreadSpecific, DestructorsRunAtThreadExitWithTheValue) {
    ASSERT_EQ(key_create(&key, record), 0);
    destroyed.clear();
    ASSERT_EQ(join(start(set_value, reinterpret_cast<void*>(0x10)), nullptr), 0);
    ASSERT_EQ(join(start(set_value_and_exit, reinterpret_cast<void*>(0x20)), nullptr), 0);
    ASSERT_EQ(join(start(set_value, nullptr), nullptr), 0);  // a null value isn't destroyed
    EXPECT_EQ(destroyed, (std::vector<std::uintptr_t>{0x10, 0x20}));
    EXPECT_EQ(key_delete(key), 0);
}

TEST(ThreadSpecific, ADestructorThatSetsAValueRunsAgainUpToFourRounds) {
    ASSERT_EQ(key_create(&key, record), 0);
    destroyed.clear();
    rearm = 10;
    ASSERT_EQ(join(start(set_value, reinterpret_cast<void*>(0x30)), nullptr), 0);
    EXPECT_EQ(destroyed, (std::vector<std::uintptr_t>{0x30, 0x31, 0x32, 0x33}));
    rearm = 0;
    EXPECT_EQ(key_delete(key), 0);
}

TEST(ThreadSpecific, ValuesArePerThread) {
    ASSERT_EQ(key_create(&key, nullptr), 0);
    int mine = 0;
    ASSERT_EQ(setspecific(key, &mine), 0);
    ASSERT_EQ(join(start(set_value, reinterpret_cast<void*>(0x40)), nullptr), 0);
    EXPECT_EQ(getspecific(key), &mine);
    EXPECT_EQ(key_delete(key), 0);
}

TEST(ThreadSpecific, ADeletedKeyIsRefusedAndItsValueDoesNotShowUnderANewKey) {
    int first = 0;
    ASSERT_EQ(key_create(&first, record), 0);
    EXPECT_NE(first, 0);
    ASSERT_EQ(setspecific(first, &first), 0);
    ASSERT_EQ(key_delete(first), 0);
    EXPECT_EQ(key_delete(first), einval);
    EXPECT_EQ(setspecific(first, &first), einval);
    EXPECT_EQ(getspecific(first), nullptr);
    int second = 0;
    ASSERT_EQ(key_create(&second, nullptr), 0);
    EXPECT_EQ(second, first);  // the slot is reused
    EXPECT_EQ(getspecific(second), nullptr);
    EXPECT_EQ(key_delete(second), 0);
}

std::atomic<bool> value_set{false};
std::atomic<bool> key_deleted{false};

void* set_value_and_wait_for_delete(void* value) {
    set_value(value);
    value_set = true;
    while (!key_deleted) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return nullptr;
}

TEST(ThreadSpecific, AKeyDeletedWhileAThreadHoldsAValueCallsNoDestructorAtItsExit) {
    ASSERT_EQ(key_create(&key, record), 0);
    destroyed.clear();
    value_set = false;
    key_deleted = false;
    Handle thread = start(set_value_and_wait_for_delete, reinterpret_cast<void*>(0x50));
    while (!value_set) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(key_delete(key), 0);
    key_deleted = true;
    ASSERT_EQ(join(thread, nullptr), 0);
    EXPECT_TRUE(destroyed.empty());
}

TEST(ThreadSpecific, At256KeysCreateFailsWithEagain) {
    std::vector<int> created;
    int k = 0;
    std::int32_t result = 0;
    while ((result = key_create(&k, nullptr)) == 0 && created.size() <= 256) {
        created.push_back(k);
    }
    EXPECT_EQ(result, eagain);
    EXPECT_EQ(created.size(), 256u);
    for (const int c : created) {
        EXPECT_EQ(key_delete(c), 0);
    }
    ASSERT_EQ(key_create(&k, nullptr), 0);  // a freed slot can be used again
    EXPECT_EQ(key_delete(k), 0);
}

TEST(ThreadSpecific, InvalidKeysAreRefused) {
    EXPECT_EQ(key_create(nullptr, nullptr), einval);
    for (const int bad : {0, -1, 100000}) {
        EXPECT_EQ(setspecific(bad, nullptr), einval);
        EXPECT_EQ(getspecific(bad), nullptr);
        EXPECT_EQ(key_delete(bad), einval);
    }
}

// ---- once ------------------------------------------------------------------------------------

std::atomic<int> once_runs{0};
std::atomic<bool> once_running{false};

void slow_routine() {
    once_running = true;
    ++once_runs;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    once_running = false;
}

struct Control {
    int state = 0;
    void* mutex = nullptr;  // the guest's control is 16 bytes, zero-filled
};
Control shared_control;

void* call_once(void*) {
    EXPECT_EQ(once(&shared_control.state, slow_routine), 0);
    EXPECT_FALSE(once_running);  // every caller returns only after the routine ended
    return nullptr;
}

TEST(Once, RunsTheRoutineOnceAcrossThreads) {
    std::vector<Handle> threads;
    for (int i = 0; i < 8; ++i) {
        threads.push_back(start(call_once));
    }
    for (Handle thread : threads) {
        EXPECT_EQ(join(thread, nullptr), 0);
    }
    EXPECT_EQ(once_runs, 1);
    call_once(nullptr);
    EXPECT_EQ(once_runs, 1);
}

Control outer, inner;
std::atomic<int> nested_runs{0};
void inner_routine() {
    ++nested_runs;
}
void outer_routine() {
    EXPECT_EQ(once(&inner.state, inner_routine), 0);
}

TEST(Once, ARoutineCanUseAnotherControl) {
    EXPECT_EQ(once(&outer.state, outer_routine), 0);
    EXPECT_EQ(once(&inner.state, inner_routine), 0);
    EXPECT_EQ(nested_runs, 1);
}

TEST(Once, NullArgumentsAreRefused) {
    Control control;
    EXPECT_EQ(once(nullptr, inner_routine), einval);
    EXPECT_EQ(once(&control.state, nullptr), einval);
}

}  // namespace
