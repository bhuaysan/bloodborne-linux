#include "launcher/find_game.hpp"

#include <stdlib.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "common/sha256.hpp"

namespace {

namespace fs = std::filesystem;
using namespace bb::launcher;

constexpr const char* fake_eboot = "not a real eboot";

std::string fake_hash() {
    const std::string text = fake_eboot;
    return bb::common::to_hex(bb::common::sha256(std::as_bytes(std::span(text))));
}

std::string make_sfo(const std::vector<std::pair<std::string, std::string>>& entries) {
    std::string keys;
    std::string data;
    std::string table;
    const auto put32 = [](std::string& s, std::uint32_t v) { s.append(reinterpret_cast<char*>(&v), 4); };
    const auto put16 = [](std::string& s, std::uint16_t v) { s.append(reinterpret_cast<char*>(&v), 2); };
    for (const auto& [k, v] : entries) {
        put16(table, static_cast<std::uint16_t>(keys.size()));
        put16(table, 0x0204);
        put32(table, static_cast<std::uint32_t>(v.size() + 1));
        put32(table, static_cast<std::uint32_t>(v.size() + 1));
        put32(table, static_cast<std::uint32_t>(data.size()));
        keys += k + '\0';
        data += v + '\0';
    }
    const std::uint32_t key_table = 20 + static_cast<std::uint32_t>(table.size());
    const std::uint32_t data_table = key_table + static_cast<std::uint32_t>(keys.size());
    std::string out("\0PSF", 4);
    put32(out, 0x101);
    put32(out, key_table);
    put32(out, data_table);
    put32(out, static_cast<std::uint32_t>(entries.size()));
    return out + table + keys + data;
}

class FindGameTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::string templ = (fs::temp_directory_path() / "bb-find-test-XXXXXX").string();
        ASSERT_NE(::mkdtemp(templ.data()), nullptr);
        root_ = fs::canonical(templ);
    }
    void TearDown() override { fs::remove_all(root_); }

    void game(const fs::path& rel, const std::string& title = "CUSA03173") {
        const fs::path dir = root_ / rel;
        fs::create_directories(dir / "sce_sys");
        std::ofstream(dir / "eboot.bin") << fake_eboot;
        std::ofstream(dir / "sce_sys/param.sfo", std::ios::binary)
            << make_sfo({{"APP_VER", "01.09"}, {"TITLE_ID", title}, {"VERSION", "01.00"}});
    }
    FindResult find(std::vector<fs::path> roots = {}, int depth = 4,
                    std::chrono::milliseconds limit = std::chrono::milliseconds(5000)) {
        if (roots.empty()) {
            roots.push_back(root_);
        }
        FindOptions o;
        o.max_depth = depth;
        o.time_limit = limit;
        const std::string hash = fake_hash();
        o.eboot_sha256 = hash;
        return find_game_folders(roots, o, cancel_);
    }

    fs::path root_;
    std::atomic<bool> cancel_{false};
};

TEST_F(FindGameTest, OneHit) {
    game("Games/Bloodborne");
    const FindResult r = find();
    ASSERT_EQ(r.folders.size(), 1u);
    EXPECT_EQ(r.folders[0], root_ / "Games/Bloodborne");
    EXPECT_FALSE(r.timed_out);
}

TEST_F(FindGameTest, SeveralHitsShallowFirst) {
    game("a/b/c/dump");
    game("x");
    game("a/y");
    const FindResult r = find();
    ASSERT_EQ(r.folders.size(), 3u);
    EXPECT_EQ(r.folders[0], root_ / "x");
    EXPECT_EQ(r.folders[1], root_ / "a/y");
    EXPECT_EQ(r.folders[2], root_ / "a/b/c/dump");
}

TEST_F(FindGameTest, TheFourthLevelCountsAndTheFifthDoesNot) {
    game("a/b/c/d");
    game("a/b/c/d2/e");
    const FindResult r = find();
    ASSERT_EQ(r.folders.size(), 1u);
    EXPECT_EQ(r.folders[0], root_ / "a/b/c/d");
}

TEST_F(FindGameTest, HiddenDirectoriesAreSkipped) {
    game(".local/share/Bloodborne");
    game("Games/.cache/Bloodborne");
    EXPECT_TRUE(find().folders.empty());
}

TEST_F(FindGameTest, ADecoyWithAnotherTitleIdIsNoHit) {
    game("Decoy", "CUSA00001");
    game("Real");
    const FindResult r = find();
    ASSERT_EQ(r.folders.size(), 1u);
    EXPECT_EQ(r.folders[0], root_ / "Real");
}

TEST_F(FindGameTest, AFolderWithAnotherEbootIsNoHit) {
    game("Games/Bloodborne");
    std::ofstream(root_ / "Games/Bloodborne/eboot.bin") << "patched";
    EXPECT_TRUE(find().folders.empty());
}

TEST_F(FindGameTest, AGameFolderIsNotSearchedFurther) {
    game("Games/Bloodborne");
    game("Games/Bloodborne/inner");
    EXPECT_EQ(find().folders.size(), 1u);
}

TEST_F(FindGameTest, SymbolicLinksAreNotFollowed) {
    game("real/Bloodborne");
    fs::create_directory_symlink(root_ / "real", root_ / "link");
    fs::create_directory_symlink(root_, root_ / "real/loop");
    EXPECT_EQ(find().folders.size(), 1u);
}

TEST_F(FindGameTest, TwoRootsAndASharedFolderCountOnce) {
    game("Games/Bloodborne");
    game("media/Disk/Bloodborne");
    const FindResult r = find({root_, root_ / "media", root_ / "missing"});
    EXPECT_EQ(r.folders.size(), 2u);
}

TEST_F(FindGameTest, NothingToFind) {
    fs::create_directories(root_ / "a/b");
    const FindResult r = find();
    EXPECT_TRUE(r.folders.empty());
    EXPECT_FALSE(r.timed_out);
}

TEST_F(FindGameTest, TheTimeLimitEndsTheSearchOnALargeTree) {
    for (int i = 0; i < 40; ++i) {
        for (int j = 0; j < 40; ++j) {
            fs::create_directories(root_ / std::to_string(i) / std::to_string(j) / "k");
        }
    }
    game("zzz/Bloodborne");
    const FindResult r = find({}, 4, std::chrono::milliseconds(0));
    EXPECT_TRUE(r.timed_out);
    EXPECT_TRUE(r.folders.empty());
    EXPECT_LT(r.elapsed.count(), 500);
    // Without the limit the whole tree is walked.
    const FindResult full = find();
    EXPECT_FALSE(full.timed_out);
    EXPECT_EQ(full.folders.size(), 1u);
}

TEST_F(FindGameTest, CancelStopsTheSearch) {
    game("Games/Bloodborne");
    cancel_ = true;
    const FindResult r = find();
    EXPECT_TRUE(r.cancelled);
    EXPECT_TRUE(r.folders.empty());
}

TEST(FindGameRoots, HomeAndMediaMountPoints) {
    const auto roots = default_search_roots("/srv/player", "p");
    ASSERT_EQ(roots.size(), 3u);
    EXPECT_EQ(roots[0], "/srv/player");
    EXPECT_EQ(roots[1], "/run/media/p");
    EXPECT_EQ(roots[2], "/media");
    EXPECT_EQ(default_search_roots("", "").size(), 1u);
}

}  // namespace
