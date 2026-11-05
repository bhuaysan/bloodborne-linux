#include "hle/system/system.hpp"

#include <array>
#include <cstdint>
#include <cstddef>
#include <string>

#include <gtest/gtest.h>

#include "hle/hle.hpp"
#include "loader/dynamic.hpp"

namespace {

using namespace bb::hle::system;

class SystemTest : public ::testing::Test {
protected:
    void SetUp() override { reset(); }
    void TearDown() override { reset(); }
};

// Runs `f` and returns what it wrote to stderr.
template <class F>
std::string stderr_of(F f) {
    ::testing::internal::CaptureStderr();
    f();
    return ::testing::internal::GetCapturedStderr();
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

std::uintptr_t resolved(const char* library, const char* nid) {
    return bb::hle::resolve({nid, library, "libTestModule", bb::loader::stt_func});
}

template <class F>
std::uintptr_t address(F* f) {
    return reinterpret_cast<std::uintptr_t>(f);
}

TEST(SystemRegistration, EveryNidResolvesToItsImplementation) {
    struct Row {
        const char *library, *nid;
        std::uintptr_t impl;
    };
    const Row rows[] = {
        {"libSceSysmodule", "g8cM39EUZ6o", address(&sysmodule_load_module)},
        {"libSceSysmodule", "eR2bZFAAU0Q", address(&sysmodule_unload_module)},
        {"libSceSysmodule", "fMP5NHUOaMk", address(&sysmodule_is_loaded)},
        {"libSceUserService", "j3YMu1MVNNo", address(&user_service_initialize)},
        {"libSceUserService", "bwFjS+bX9mA", address(&user_service_terminate)},
        {"libSceUserService", "yH17Q6NWtVg", address(&user_service_get_event)},
        {"libSceUserService", "CdWp0oHWGr0", address(&user_service_get_user_id)},
        {"libSceUserService", "fPhymKNvK-A", address(&user_service_get_user_list)},
        {"libSceUserService", "1xxcMiGu2fo", address(&user_service_get_user_name)},
    };
    for (const Row& row : rows) {
        EXPECT_EQ(resolved(row.library, row.nid), row.impl) << row.library << " " << row.nid;
    }
}

TEST_F(SystemTest, SysmoduleTracksLoadedIds) {
    EXPECT_EQ(sysmodule_is_loaded(0xb4), sysmodule_error_not_loaded);
    const std::string log = stderr_of([] { EXPECT_EQ(sysmodule_load_module(0xb4), 0); });
    EXPECT_TRUE(
        contains(log, "hle.libSceSysmodule: load module 0xb4 (libSceAppContent) (caller 0x"))
        << log;
    EXPECT_EQ(sysmodule_is_loaded(0xb4), 0);
    EXPECT_EQ(sysmodule_is_loaded(0x83), sysmodule_error_not_loaded);
    EXPECT_EQ(sysmodule_unload_module(0xb4), 0);
    EXPECT_EQ(sysmodule_is_loaded(0xb4), sysmodule_error_not_loaded);
    EXPECT_EQ(sysmodule_unload_module(0xb4), sysmodule_error_not_loaded);
}

TEST_F(SystemTest, SysmoduleLoadsAndLogsUnknownIds) {
    const std::string log = stderr_of([] { EXPECT_EQ(sysmodule_load_module(0x1234), 0); });
    EXPECT_TRUE(contains(log, "load module 0x1234 (unknown) (caller 0x")) << log;
    EXPECT_EQ(sysmodule_is_loaded(0x1234), 0);
}

TEST_F(SystemTest, SysmoduleNamesTheIdsOfTheImportsDoc) {
    EXPECT_EQ(sysmodule_library(0x83), "libScePlayGo");
    EXPECT_EQ(sysmodule_library(0xa0), "libSceSaveData");
    EXPECT_EQ(sysmodule_library(0xd7), "libSceDiscMap");
    EXPECT_EQ(sysmodule_library(0xb5), "online subsystem");
    EXPECT_EQ(sysmodule_library(0), "unknown");
}

TEST_F(SystemTest, UserServiceQueriesNeedInitialize) {
    std::int32_t id = 0;
    std::array<std::int32_t, user_list_size> list{};
    std::array<char, 17> name{};
    EXPECT_EQ(user_service_get_user_id(&id), user_service_error_not_initialized);
    EXPECT_EQ(user_service_get_user_list(list.data()), user_service_error_not_initialized);
    EXPECT_EQ(user_service_get_user_name(user_id, name.data(), name.size()),
              user_service_error_not_initialized);
    EXPECT_EQ(user_service_terminate(), user_service_error_not_initialized);
}

TEST_F(SystemTest, UserServiceInitializesOnce) {
    EXPECT_EQ(user_service_initialize(nullptr), 0);
    EXPECT_EQ(user_service_initialize(nullptr), user_service_error_already_initialized);
    EXPECT_EQ(user_service_terminate(), 0);
    EXPECT_EQ(user_service_initialize(nullptr), 0);
}

TEST_F(SystemTest, UserServiceReportsOneUser) {
    ASSERT_EQ(user_service_initialize(nullptr), 0);
    std::int32_t id = 0;
    EXPECT_EQ(user_service_get_user_id(&id), 0);
    EXPECT_EQ(id, user_id);
    std::array<std::int32_t, user_list_size> list{};
    EXPECT_EQ(user_service_get_user_list(list.data()), 0);
    EXPECT_EQ(list, (std::array<std::int32_t, user_list_size>{user_id, -1, -1, -1}));
    std::array<std::int32_t, 4> event{};
    EXPECT_EQ(user_service_get_event(event.data()), user_service_bb_error_no_event);
}

TEST_F(SystemTest, UserNameIsTheProfileName) {
    ASSERT_EQ(user_service_initialize(nullptr), 0);
    std::array<char, 0x11> name;
    name.fill('x');
    EXPECT_EQ(user_service_get_user_name(user_id, name.data(), name.size()), 0);
    EXPECT_STREQ(name.data(), "default");

    set_user_name("hunter_of_yharnam_0123456789");  // longer than the game's 16 characters
    name.fill('x');
    EXPECT_EQ(user_service_get_user_name(user_id, name.data(), name.size()), 0);
    EXPECT_STREQ(name.data(), "hunter_of_yharna");

    const std::string log = stderr_of([&] {
        EXPECT_EQ(user_service_get_user_name(user_id + 1, name.data(), name.size()),
                  user_service_bb_error_invalid_argument);
    });
    EXPECT_TRUE(contains(log, "invalid arguments: id 2 size 17 (caller 0x")) << log;
    EXPECT_EQ(user_service_get_user_name(user_id, name.data(), 0),
              user_service_bb_error_invalid_argument);
}

}  // namespace
