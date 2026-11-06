#include "hle/kernel/paths.hpp"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>

#include <stdlib.h>
#include <sys/stat.h>

#include <gtest/gtest.h>

#include "common/runtime_dirs.hpp"
#include "support/game.hpp"
#include "hle/kernel/abi.hpp"

namespace {

namespace fs = std::filesystem;
using namespace bb::hle::kernel;

int guest(int host) {
    return guest_errno(host).value();
}

void touch(const fs::path& file) {
    fs::create_directories(file.parent_path());
    std::ofstream(file) << "x";
}

// A temporary game directory with a few files, a junk file next to each, and a save data
// directory, all removed afterwards.
class PathMapTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::string templ = (fs::temp_directory_path() / "bb-paths-test-XXXXXX").string();
        ASSERT_NE(::mkdtemp(templ.data()), nullptr);
        tmp_ = fs::canonical(templ);
        game_ = tmp_ / "game";
        touch(game_ / "eboot.bin");
        touch(game_ / "._eboot.bin");
        touch(game_ / ".DS_Store");
        touch(game_ / "dvdroot_ps4" / "Map" / "m10_00.txt");
        touch(game_ / "dvdroot_ps4" / "._map" / "m10_00.txt");
        save_ = tmp_ / "save";
        fs::create_directories(save_);
        map_.mount_game(game_);
    }
    void TearDown() override { fs::remove_all(tmp_); }

    fs::path tmp_, game_, save_;
    PathMap map_;
};

TEST(PathNames, JunkFiles) {
    EXPECT_TRUE(is_junk_name("._eboot.bin"));
    EXPECT_TRUE(is_junk_name(".DS_Store"));
    EXPECT_FALSE(is_junk_name("eboot.bin"));
    EXPECT_FALSE(is_junk_name(".hidden"));
    EXPECT_FALSE(is_junk_name("."));
}

TEST_F(PathMapTest, GameMountIsReadOnly) {
    Resolved r = map_.resolve("/app0/eboot.bin", Access::read);
    ASSERT_TRUE(r);
    EXPECT_EQ(r.host, game_ / "eboot.bin");
    r = map_.resolve("/app0", Access::read);
    ASSERT_TRUE(r);
    EXPECT_EQ(r.host, game_);
    // A path that doesn't exist still resolves; the host call reports ENOENT.
    r = map_.resolve("/app0/missing/file", Access::read);
    ASSERT_TRUE(r);
    EXPECT_EQ(r.host, game_ / "missing" / "file");
    EXPECT_EQ(map_.resolve("/app0/eboot.bin", Access::write).error, guest(EROFS));
    EXPECT_EQ(map_.resolve("/app0/new.txt", Access::write).error, guest(EROFS));
    EXPECT_EQ(map_.resolve("/app0", Access::write).error, guest(EROFS));
}

TEST_F(PathMapTest, Normalisation) {
    const Resolved r = map_.resolve("//app0/./dvdroot_ps4//Map/../Map/./m10_00.txt", Access::read);
    ASSERT_TRUE(r);
    EXPECT_EQ(r.host, game_ / "dvdroot_ps4" / "Map" / "m10_00.txt");
    EXPECT_EQ(map_.resolve("/app0/dvdroot_ps4/..", Access::read).host, game_);
}

TEST_F(PathMapTest, TrailingSlashNamesADirectory) {
    EXPECT_EQ(map_.resolve("/app0/dvdroot_ps4/Map/", Access::read).host,
              game_ / "dvdroot_ps4" / "Map" / "");
    EXPECT_EQ(map_.resolve("/app0/dvdroot_ps4/.", Access::read).host, game_ / "dvdroot_ps4" / "");
    // On a file the host call fails with ENOTDIR, as on the guest.
    const Resolved r = map_.resolve("/app0/eboot.bin/", Access::read);
    ASSERT_TRUE(r);
    EXPECT_EQ(r.host, game_ / "eboot.bin" / "");
    struct stat st;
    EXPECT_EQ(::stat(r.host.c_str(), &st), -1);
    EXPECT_EQ(errno, ENOTDIR);
}

TEST_F(PathMapTest, DotDotNeverLeavesTheMount) {
    ASSERT_TRUE(map_.mount("/savedata0", save_));
    EXPECT_EQ(map_.resolve("/app0/..", Access::read).error, guest(ENOENT));
    EXPECT_EQ(map_.resolve("/app0/../savedata0/x", Access::read).error, guest(ENOENT));
    EXPECT_EQ(map_.resolve("/app0/dvdroot_ps4/../../eboot.bin", Access::read).error,
              guest(ENOENT));
    EXPECT_EQ(map_.resolve("/savedata0/../../etc/passwd", Access::write).error, guest(ENOENT));
}

TEST_F(PathMapTest, JunkFilesNeverResolve) {
    EXPECT_EQ(map_.resolve("/app0/dvdroot_ps4/._map/m10_00.txt", Access::read).error,
              guest(ENOENT));
    EXPECT_EQ(map_.resolve("/app0/._eboot.bin", Access::read).error, guest(ENOENT));
    EXPECT_EQ(map_.resolve("/app0/.DS_Store", Access::read).error, guest(ENOENT));
}

TEST_F(PathMapTest, WritableMountsAndUnmount) {
    ASSERT_TRUE(map_.mount("/savedata0", save_));
    EXPECT_FALSE(map_.mount("/savedata0", tmp_));
    Resolved r = map_.resolve("/savedata0/sub/userdata0010", Access::write);
    ASSERT_TRUE(r);
    EXPECT_EQ(r.host, save_ / "sub" / "userdata0010");
    EXPECT_TRUE(map_.unmount("/savedata0"));
    EXPECT_FALSE(map_.unmount("/savedata0"));
    EXPECT_FALSE(map_.unmount("/app0"));
    EXPECT_EQ(map_.resolve("/savedata0/x", Access::read).error, guest(ENOENT));
}

TEST_F(PathMapTest, NamesTheMountAndItsRoot) {
    ASSERT_TRUE(map_.mount("/savedata0", save_));
    Resolved r = map_.resolve("//savedata0/./sub/", Access::write);
    EXPECT_EQ(r.mount, "savedata0");
    EXPECT_FALSE(r.mount_root);
    r = map_.resolve("/savedata0/sub/..", Access::write);
    EXPECT_EQ(r.mount, "savedata0");
    EXPECT_TRUE(r.mount_root);
    r = map_.resolve("/app0/", Access::read);
    EXPECT_EQ(r.mount, "app0");
    EXPECT_TRUE(r.mount_root);
    r = map_.resolve("/nowhere/x", Access::read);
    EXPECT_EQ(r.mount, "");
    EXPECT_FALSE(r.mount_root);
}

TEST_F(PathMapTest, MalformedOrReservedMountPointsThrow) {
    for (const char* point : {"", "savedata0", "/", "/a/b", "/..", "/.", "/app0", "/hostapp"}) {
        EXPECT_THROW(map_.mount(point, save_), std::invalid_argument) << point;
    }
}

TEST_F(PathMapTest, UnknownPrefixesAreRefusedAndLoggedOnce) {
    testing::internal::CaptureStderr();
    EXPECT_EQ(map_.resolve("/data/a", Access::read).error, guest(ENOENT));
    EXPECT_EQ(map_.resolve("/data/b", Access::write).error, guest(ENOENT));
    EXPECT_EQ(map_.resolve("/hostapp/log.txt", Access::write).error, guest(ENOENT));
    EXPECT_EQ(map_.resolve("/", Access::read).error, guest(ENOENT));
    EXPECT_EQ(map_.resolve("", Access::read).error, guest(ENOENT));
    EXPECT_EQ(map_.resolve("app0/eboot.bin", Access::read).error, guest(ENOENT));
    const std::string log = testing::internal::GetCapturedStderr();
    EXPECT_EQ(log.find("/data"), log.rfind("/data")) << log;
    EXPECT_NE(log.find("/data"), std::string::npos) << log;
    EXPECT_NE(log.find("/hostapp"), std::string::npos) << log;
}

TEST_F(PathMapTest, LongNames) {
    EXPECT_EQ(map_.resolve("/app0/" + std::string(256, 'a'), Access::read).error,
              guest(ENAMETOOLONG));
    EXPECT_TRUE(map_.resolve("/app0/" + std::string(255, 'a'), Access::read));
    // PATH_MAX is 1024 including the NUL: 1023 bytes resolve, 1024 fail.
    const auto path_of_size = [](std::size_t size) {
        std::string path = "/app0";
        while (size - path.size() > 201) {
            path += "/" + std::string(200, 'a');
        }
        path += "/" + std::string(size - path.size() - 1, 'b');
        return path;
    };
    const std::string longest = path_of_size(1023);
    ASSERT_EQ(longest.size(), 1023u);
    EXPECT_TRUE(map_.resolve(longest, Access::read));
    const std::string too_long = path_of_size(1024);
    ASSERT_EQ(too_long.size(), 1024u);
    EXPECT_EQ(map_.resolve(too_long, Access::read).error, guest(ENAMETOOLONG));
}

TEST_F(PathMapTest, ProfileDirectories) {
    const char* keys[] = {"XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME"};
    std::optional<std::string> saved[4];
    for (int i = 0; i < 4; ++i) {
        if (const char* v = std::getenv(keys[i])) {
            saved[i] = v;
        }
        ::setenv(keys[i], (tmp_ / "xdg" / keys[i]).c_str(), 1);
    }
    const auto dirs = bb::common::RuntimeDirs::from_environment("p1", {game_});
    for (int i = 0; i < 4; ++i) {
        if (saved[i]) {
            ::setenv(keys[i], saved[i]->c_str(), 1);
        } else {
            ::unsetenv(keys[i]);
        }
    }
    const fs::path save = savedata_dir(dirs, 3);
    EXPECT_EQ(save, dirs.profile_data() / "savedata" / "3");
    EXPECT_TRUE(fs::is_directory(save));
    const fs::path temp = temp_dir(dirs);
    EXPECT_EQ(temp, dirs.profile_cache() / "temp");
    EXPECT_TRUE(fs::is_directory(temp));
    ASSERT_TRUE(map_.mount("/savedata3", save));
    EXPECT_EQ(map_.resolve("/savedata3/f", Access::write).host, save / "f");
}

TEST(PathMapGlobal, ResolvePathUsesTheProcessTable) {
    EXPECT_EQ(&path_map(), &path_map());
    EXPECT_EQ(resolve_path("/nowhere/x", Access::read).error, guest(ENOENT));
}

TEST(PathMapGame, DvdrootResolvesToAnExistingDirectory) {
    BB_REQUIRE_GAME(game);
    PathMap map;
    map.mount_game(game);
    // The eboot's path string at 0x49be4c4 (UTF-16).
    const Resolved r = map.resolve("/app0/dvdroot_ps4/", Access::read);
    ASSERT_TRUE(r);
    EXPECT_TRUE(fs::is_directory(r.host)) << r.host;
}

}  // namespace
