#include "launcher/profiles.hpp"
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

namespace {

using namespace bb::launcher;
namespace fs = std::filesystem;

class ProfilesTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() / ("bb-profiles-test-" + std::to_string(::getpid()));
        fs::remove_all(root_);
        data_ = root_ / "data";
        cache_ = root_ / "cache";
        fs::create_directories(data_);
    }
    void TearDown() override { fs::remove_all(root_); }
    void touch(const fs::path& file, int age_seconds) {
        fs::create_directories(file.parent_path());
        std::ofstream(file) << "x";
        fs::last_write_time(file, fs::file_time_type::clock::now() - std::chrono::seconds(age_seconds));
    }
    fs::path root_, data_, cache_;
};

TEST_F(ProfilesTest, NamingRules) {
    EXPECT_EQ(check_profile_name(""), NameProblem::empty);
    EXPECT_EQ(check_profile_name(std::string(33, 'a')), NameProblem::too_long);
    EXPECT_EQ(check_profile_name(std::string(32, 'a')), NameProblem::ok);
    EXPECT_EQ(check_profile_name("my save"), NameProblem::bad_character);
    EXPECT_EQ(check_profile_name("a/b"), NameProblem::bad_character);
    EXPECT_EQ(check_profile_name(".."), NameProblem::bad_character);
    EXPECT_EQ(check_profile_name("sp\xc3\xa4t"), NameProblem::bad_character);
    EXPECT_EQ(check_profile_name("Test_1-b"), NameProblem::ok);
}

TEST_F(ProfilesTest, ListsNewestFirstAndSkipsOddEntries) {
    touch(data_ / "profiles/old/savedata/a", 5000);
    touch(data_ / "profiles/new/savedata/deep/b", 10);
    touch(data_ / "profiles/not valid/x", 1);
    touch(data_ / "profiles/stray-file", 1);
    const auto list = list_profiles(data_, "default");
    ASSERT_EQ(list.size(), 3u);
    EXPECT_EQ(list[0].name, "new");
    EXPECT_EQ(list[1].name, "old");
    EXPECT_EQ(list[2].name, "default");  // in use but never run: listed, no time
    EXPECT_FALSE(list[2].last_played.has_value());
    ASSERT_TRUE(list[0].last_played.has_value());
    EXPECT_GT(*list[0].last_played, *list[1].last_played);
}

TEST_F(ProfilesTest, NoProfilesFolderGivesOnlyTheCurrent) {
    const auto list = list_profiles(data_, "default");
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].name, "default");
    EXPECT_TRUE(list_profiles(data_, "").empty());
}

TEST_F(ProfilesTest, CreatesAndRefusesDuplicates) {
    EXPECT_EQ(create_profile(data_, "test"), NameProblem::ok);
    EXPECT_TRUE(fs::is_directory(profile_folder(data_, "test")));
    EXPECT_EQ(create_profile(data_, "test"), NameProblem::exists);
    EXPECT_EQ(create_profile(data_, "bad name"), NameProblem::bad_character);
    EXPECT_FALSE(fs::exists(data_ / "profiles" / "bad name"));
}

TEST_F(ProfilesTest, RenamesDataAndCache) {
    touch(data_ / "profiles/a/savedata/s", 1);
    touch(cache_ / "profiles/a/tmp", 1);
    touch(data_ / "profiles/b/x", 1);
    EXPECT_EQ(rename_profile(data_, cache_, "a", "b"), NameProblem::exists);
    EXPECT_EQ(rename_profile(data_, cache_, "a", "no way"), NameProblem::bad_character);
    EXPECT_TRUE(fs::exists(data_ / "profiles/a/savedata/s"));
    EXPECT_EQ(rename_profile(data_, cache_, "a", "c"), NameProblem::ok);
    EXPECT_TRUE(fs::exists(data_ / "profiles/c/savedata/s"));
    EXPECT_FALSE(fs::exists(data_ / "profiles/a"));
    EXPECT_TRUE(fs::exists(cache_ / "profiles/c/tmp"));
    // A profile without a folder renames without moving anything.
    EXPECT_EQ(rename_profile(data_, cache_, "ghost", "d"), NameProblem::ok);
    EXPECT_FALSE(fs::exists(data_ / "profiles/d"));
}

TEST(ProfilesUrl, EncodesTheFolder) {
    EXPECT_EQ(file_url("/srv/data/bloodborne-linux/profiles/test"),
              "file:///srv/data/bloodborne-linux/profiles/test");
    EXPECT_EQ(file_url("/a b/c#d"), "file:///a%20b/c%23d");
}

}  // namespace
