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
        {"libSceSystemService", "rPo6tV8D9bM", address(&system_service_get_status)},
        {"libSceSystemService", "656LMQSrg6U", address(&system_service_receive_event)},
        {"libSceSystemService", "fZo48un7LK4", address(&system_service_get_int)},
        {"libSceSystemService", "-+3hY+y8bNo", address(&system_service_open_url)},
        {"libSceSystemService", "Vo5V8KAwCmk", address(&system_service_notify)},
        {"libSceAppContent", "R9lA82OraNs", address(&app_content_initialize)},
        {"libSceAppContent", "99b82IKXpH4", address(&app_content_get_int)},
        {"libSceAppContent", "xnd8BJzAxmk", address(&app_content_get_addcont_count)},
        {"libScePlayGo", "ts6GlZOKRrE", address(&playgo_initialize)},
        {"libScePlayGo", "M1Gma1ocrGE", address(&playgo_open)},
        {"libScePlayGo", "73fF1MFU8hA", address(&playgo_get_chunk_ids)},
        {"libScePlayGo", "uWIYLFkkwqk", address(&playgo_get_chunk_states)},
        {"libScePlayGo", "4AAcTU9R3XM", address(&playgo_set_mode)},
        {"libSceDiscMap", "lbQKqsERhtE", address(&disc_map_is_remapped)},
        {"libSceDiscMap", "ioKMruft1ek", address(&disc_map_map_range)},
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

TEST_F(SystemTest, SysmoduleLogsUnknownIdsInIsLoaded) {
    const std::string unknown = stderr_of([] {
        EXPECT_EQ(sysmodule_is_loaded(0x1234), sysmodule_error_not_loaded);
    });
    EXPECT_TRUE(contains(unknown, "is loaded 0x1234 (unknown) (caller 0x")) << unknown;
    const std::string known = stderr_of([] {
        EXPECT_EQ(sysmodule_is_loaded(0xa0), sysmodule_error_not_loaded);
    });
    EXPECT_FALSE(contains(known, "is loaded")) << known;
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

TEST_F(SystemTest, SystemServiceStatusHasNoEvents) {
    std::array<std::int32_t, 4> status;
    status.fill(-1);
    EXPECT_EQ(system_service_get_status(status.data()), 0);
    EXPECT_EQ(status[0], 0);
    EXPECT_EQ(status[1], -1);  // only the event count is written
    std::array<std::byte, 64> event{};
    EXPECT_EQ(system_service_receive_event(event.data()), system_service_bb_error_no_event);
}

TEST_F(SystemTest, SystemServiceIntegerQuery) {
    std::int32_t value = -1;
    EXPECT_EQ(system_service_get_int(1, &value), 0);
    EXPECT_EQ(value, system_language_english_us);

    value = -1;
    const std::string log = stderr_of([&] {
        EXPECT_EQ(system_service_get_int(7, &value), system_service_bb_error_unknown_id);
    });
    EXPECT_EQ(value, -1);
    EXPECT_TRUE(contains(log, "hle.libSceSystemService: integer query: unknown id 7 (caller 0x"))
        << log;
}

TEST_F(SystemTest, SystemServiceRequestsSucceed) {
    const std::string log = stderr_of([] {
        EXPECT_EQ(system_service_open_url("http://example.invalid/", 0), 0);
        EXPECT_EQ(system_service_notify(), 0);
    });
    EXPECT_TRUE(contains(log, "open url \"http://example.invalid/\" 0: ignored")) << log;
    EXPECT_TRUE(contains(log, "Vo5V8KAwCmk: ignored")) << log;
}

TEST_F(SystemTest, AppContentParameters) {
    std::array<std::byte, 0x20> init{}, boot{};
    EXPECT_EQ(app_content_initialize(init.data(), boot.data()), 0);
    for (std::int32_t key = 0; key < app_param_keys; ++key) {
        std::int32_t value = -1;
        EXPECT_EQ(app_content_get_int(key, &value), 0);
        EXPECT_EQ(value, key == 1 ? 13 : 0) << key;
    }
    std::int32_t value = -1;
    const std::string log = stderr_of([&] {
        EXPECT_EQ(app_content_get_int(5, &value), app_content_bb_error_unknown_key);
        EXPECT_EQ(app_content_get_int(-1, &value), app_content_bb_error_unknown_key);
    });
    EXPECT_EQ(value, -1);
    EXPECT_TRUE(contains(log, "integer parameter: unknown key 5 (caller 0x")) << log;
    EXPECT_TRUE(contains(log, "integer parameter: unknown key -1 (caller 0x")) << log;
}

TEST_F(SystemTest, AppContentHasNoAddcont) {
    std::uint32_t count = 7;
    std::array<std::byte, 64> list{};
    EXPECT_EQ(app_content_get_addcont_count(0, nullptr, 0, &count), 0);
    EXPECT_EQ(count, 0u);
    count = 7;
    EXPECT_EQ(app_content_get_addcont_count(0, list.data(), 2, &count), 0);
    EXPECT_EQ(count, 0u);
}

TEST_F(SystemTest, PlayGoNeedsOpen) {
    std::uint32_t count = 0;
    std::array<std::uint16_t, 100> ids{};
    EXPECT_EQ(playgo_get_chunk_ids(playgo_handle, ids.data(), 100, &count),
              playgo_bb_error_bad_handle);
    std::int32_t handle = 0;
    EXPECT_EQ(playgo_initialize(nullptr), 0);
    EXPECT_EQ(playgo_open(&handle, nullptr), 0);
    EXPECT_EQ(handle, playgo_handle);
    EXPECT_EQ(playgo_get_chunk_ids(handle + 1, ids.data(), 100, &count),
              playgo_bb_error_bad_handle);
}

TEST_F(SystemTest, PlayGoReportsEverythingInstalled) {
    std::int32_t handle = 0;
    ASSERT_EQ(playgo_open(&handle, nullptr), 0);
    std::array<std::uint16_t, 100> ids;
    ids.fill(0xffff);
    std::uint32_t count = 0;
    EXPECT_EQ(playgo_get_chunk_ids(handle, ids.data(), 100, &count), 0);
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(ids[0], playgo_chunk_id);
    EXPECT_EQ(ids[1], 0xffff);
    EXPECT_EQ(playgo_get_chunk_ids(handle, ids.data(), 0, &count), 0);
    EXPECT_EQ(count, 0u);

    const std::array<std::uint16_t, 2> query{playgo_chunk_id, 5};
    std::array<std::int8_t, 3> states{-1, -1, -1};
    const std::string log = stderr_of([&] {
        EXPECT_EQ(playgo_get_chunk_states(handle, query.data(), 2, states.data()), 0);
    });
    EXPECT_EQ(states,
              (std::array<std::int8_t, 3>{playgo_chunk_installed, playgo_chunk_installed, -1}));
    // The game's fold (0x1fdeb40 to 0x1fdeb6d) counts the chunks as done only when
    // no state is 0 (missing) or 2 (pending), so the reported state must be neither.
    static_assert(playgo_chunk_installed != 0 && playgo_chunk_installed != 2);
    EXPECT_TRUE(contains(log, "chunk states: unknown chunk id 5 (caller 0x")) << log;
    EXPECT_EQ(playgo_set_mode(handle, 1), 0);
}

TEST_F(SystemTest, DiscMapRemapsNothing) {
    std::int32_t out = -1;
    EXPECT_EQ(disc_map_is_remapped("/app0/dvdroot_ps4/x", 0, 0, &out), 0);
    EXPECT_EQ(out, 0);
    std::uint64_t a = 1, b = 2, c = 3;
    EXPECT_EQ(disc_map_map_range("/app0/dvdroot_ps4/x", 0x10000, 0x800, &a, &b, &c),
              disc_map_bb_error_not_mapped);
    EXPECT_EQ(a, 1u);
    EXPECT_EQ(b, 2u);
    EXPECT_EQ(c, 3u);
}

}  // namespace
