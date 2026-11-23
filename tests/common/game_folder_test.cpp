#include "common/game_folder.hpp"

#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "common/config.hpp"
#include "common/sha256.hpp"
#include "loader/self.hpp"  // eboot_bin_sha256: the copy in game_folder.hpp must match it
#include "support/game.hpp"

namespace {

namespace fs = std::filesystem;
using bb::common::FolderCheck;

constexpr const char* fake_eboot = "not a real eboot";

std::string slurp(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string fake_hash() {
    const std::string text = fake_eboot;
    return bb::common::to_hex(bb::common::sha256(std::as_bytes(std::span(text))));
}

// A param.sfo with string entries.
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

class GameFolderTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::string templ = (fs::temp_directory_path() / "bb-folder-test-XXXXXX").string();
        ASSERT_NE(::mkdtemp(templ.data()), nullptr);
        root_ = templ;
        game_ = root_ / "game";
    }
    void TearDown() override { fs::remove_all(root_); }

    void write(const fs::path& file, const std::string& bytes) {
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << bytes;
    }
    // A complete tree; `title` and `version` go to param.sfo.
    void make_game(const std::string& title = "CUSA03173", const std::string& version = "01.09",
                   const std::string& eboot = fake_eboot) {
        write(game_ / "eboot.bin", eboot);
        write(game_ / "sce_sys/param.sfo",
              make_sfo({{"APP_VER", version},
                        {"TITLE", "Test Title"},
                        {"TITLE_ID", title},
                        {"VERSION", "01.00"}}));
    }
    FolderCheck kind() { return bb::common::check_game_folder(game_, fake_hash()).kind; }
    std::string message() { return bb::common::check_game_folder(game_, fake_hash()).message; }

    fs::path root_;
    fs::path game_;
};

TEST(GameFolder, TheDefaultHashIsThePinnedOne) {
    EXPECT_EQ(bb::common::default_eboot_sha256, bb::loader::eboot_bin_sha256);
}

TEST_F(GameFolderTest, AcceptsACompleteFolder) {
    make_game();
    const auto r = bb::common::check_game_folder(game_, fake_hash());
    EXPECT_TRUE(r.ok()) << r.message;
    EXPECT_EQ(r.title_id, "CUSA03173");
    EXPECT_EQ(r.title, "Test Title");
    EXPECT_EQ(r.app_version, "01.09");
}

TEST_F(GameFolderTest, ANewerPatchPassesTheVersionCheck) {
    make_game("CUSA03173", "01.10");
    EXPECT_EQ(kind(), FolderCheck::ok);
}

TEST_F(GameFolderTest, NotAFolder) {
    EXPECT_EQ(bb::common::check_game_folder(root_ / "missing").kind, FolderCheck::not_a_folder);
    write(root_ / "file", "x");
    EXPECT_EQ(bb::common::check_game_folder(root_ / "file").kind, FolderCheck::not_a_folder);
}

TEST_F(GameFolderTest, NoEbootInAnEmptyFolder) {
    fs::create_directories(game_);
    EXPECT_EQ(kind(), FolderCheck::no_eboot);
    EXPECT_NE(message().find("no eboot.bin"), std::string::npos);
}

TEST_F(GameFolderTest, NoEbootInTheParentNamesTheFolderInside) {
    make_game();
    const auto r = bb::common::check_game_folder(root_, fake_hash());
    EXPECT_EQ(r.kind, FolderCheck::no_eboot);
    EXPECT_NE(r.message.find("\"game\" inside it"), std::string::npos) << r.message;
}

TEST_F(GameFolderTest, NoEbootInASubfolderNamesTheFolderAbove) {
    make_game();
    const auto r = bb::common::check_game_folder(game_ / "sce_sys", fake_hash());
    EXPECT_EQ(r.kind, FolderCheck::no_eboot);
    EXPECT_NE(r.message.find("folder above"), std::string::npos) << r.message;
}

TEST_F(GameFolderTest, MissingParamSfo) {
    write(game_ / "eboot.bin", fake_eboot);
    EXPECT_EQ(kind(), FolderCheck::no_param_sfo);
}

TEST_F(GameFolderTest, UnreadableParamSfo) {
    write(game_ / "eboot.bin", fake_eboot);
    write(game_ / "sce_sys/param.sfo", "garbage");
    EXPECT_EQ(kind(), FolderCheck::bad_param_sfo);
    write(game_ / "sce_sys/param.sfo", make_sfo({{"TITLE", "No id"}}));
    EXPECT_EQ(kind(), FolderCheck::bad_param_sfo);
    std::string cut = make_sfo({{"TITLE_ID", "CUSA03173"}, {"APP_VER", "01.09"}});
    cut.resize(cut.size() - 12);  // the data table is cut short
    write(game_ / "sce_sys/param.sfo", cut);
    EXPECT_EQ(kind(), FolderCheck::bad_param_sfo);
    make_game("CUSA03173", "nine");
    EXPECT_EQ(kind(), FolderCheck::bad_param_sfo);
}

TEST_F(GameFolderTest, AnotherTitle) {
    make_game("CUSA00001");
    EXPECT_EQ(kind(), FolderCheck::other_title);
    EXPECT_NE(message().find("CUSA00001"), std::string::npos);
}

TEST_F(GameFolderTest, TheBaseGameWithoutTheUpdate) {
    make_game("CUSA03173", "01.00");
    EXPECT_EQ(kind(), FolderCheck::missing_update);
    EXPECT_NE(message().find("01.00"), std::string::npos);
    make_game("CUSA03173", "01.08");
    EXPECT_EQ(kind(), FolderCheck::missing_update);
}

TEST_F(GameFolderTest, UnknownEboot) {
    make_game("CUSA03173", "01.09", "another eboot");
    EXPECT_EQ(kind(), FolderCheck::unknown_eboot);
}

TEST_F(GameFolderTest, SetupWritesTheFolderAndKeepsOtherSettings) {
    make_game();
    const fs::path config = root_ / "cfg/config.toml";
    bb::common::Config old;
    old.fsr = true;
    old.profile = "keep";
    bb::common::write_config(config, old);
    const auto r = bb::common::setup_game_folder(game_, config, fake_hash());
    ASSERT_TRUE(r.ok()) << r.message();
    const auto read = bb::common::read_config(config).config;
    EXPECT_EQ(read.game_dir, fs::canonical(game_));
    EXPECT_EQ(read.fsr, true);
    EXPECT_EQ(read.profile, "keep");
}

TEST_F(GameFolderTest, SetupWritesNothingWhenTheCheckFails) {
    make_game("CUSA00001");
    const fs::path config = root_ / "cfg/config.toml";
    const auto r = bb::common::setup_game_folder(game_, config, fake_hash());
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.folder.kind, FolderCheck::other_title);
    EXPECT_EQ(r.message(), r.folder.message);
    EXPECT_FALSE(fs::exists(config));
}

TEST_F(GameFolderTest, SetupReportsAConfigFileThatDoesNotParse) {
    make_game();
    const fs::path config = root_ / "config.toml";
    write(config, "fsr = maybe\n");
    const auto r = bb::common::setup_game_folder(game_, config, fake_hash());
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(r.folder.ok());
    EXPECT_FALSE(r.config_written);
    EXPECT_NE(r.message().find("fsr"), std::string::npos);
}

// ---- the hash cache ---------------------------------------------------------------------------

class GameFolderCacheTest : public GameFolderTest {
protected:
    fs::path cache() const { return root_ / "cache/game-check.txt"; }
    fs::path eboot() const { return game_ / "eboot.bin"; }
    FolderCheck check() { return bb::common::check_game_folder(game_, fake_hash(), cache()).kind; }
    // Rewrites eboot.bin with bytes of the same size and puts its modification time back.
    void replace_keeping_mtime(const std::string& bytes) {
        const auto time = fs::last_write_time(eboot());
        std::fstream(eboot(), std::ios::binary | std::ios::in | std::ios::out) << bytes;
        fs::last_write_time(eboot(), time);
    }
};

TEST_F(GameFolderCacheTest, APassingCheckWritesTheRecordOfTheFile) {
    make_game();
    EXPECT_FALSE(fs::exists(cache()));
    ASSERT_EQ(check(), FolderCheck::ok);
    EXPECT_EQ(slurp(cache()), bb::common::game_check_record(eboot(), fake_hash()));
    EXPECT_FALSE(fs::exists(cache().string() + ".tmp"));
}

TEST_F(GameFolderCacheTest, AMatchingRecordSkipsTheHash) {
    make_game("CUSA03173", "01.09", "NOT A REAL EBOOT");  // fails the hash
    write(cache(), bb::common::game_check_record(eboot(), fake_hash()));
    EXPECT_EQ(check(), FolderCheck::ok) << "the record answered, so no hash ran";
    // Without the cache the same file fails.
    EXPECT_EQ(bb::common::check_game_folder(game_, fake_hash()).kind, FolderCheck::unknown_eboot);
}

TEST_F(GameFolderCacheTest, AChangedModificationTimeHashesAgain) {
    make_game();
    ASSERT_EQ(check(), FolderCheck::ok);
    const std::string old = slurp(cache());
    fs::last_write_time(eboot(), fs::last_write_time(eboot()) + std::chrono::nanoseconds(1));
    EXPECT_NE(bb::common::game_check_record(eboot(), fake_hash()), old);
    replace_keeping_mtime("NOT A REAL EBOOT");
    fs::last_write_time(eboot(), fs::last_write_time(eboot()) + std::chrono::nanoseconds(1));
    EXPECT_EQ(check(), FolderCheck::unknown_eboot);
    EXPECT_EQ(slurp(cache()), old) << "a failure leaves the cache alone";
}

TEST_F(GameFolderCacheTest, AChangedSizeHashesAgain) {
    make_game();
    ASSERT_EQ(check(), FolderCheck::ok);
    const auto time = fs::last_write_time(eboot());
    write(eboot(), std::string(fake_eboot) + "!");
    fs::last_write_time(eboot(), time);
    EXPECT_EQ(check(), FolderCheck::unknown_eboot);
}

TEST_F(GameFolderCacheTest, ChangedBytesUnderARestoredModificationTimeHashAgain) {
    make_game();
    ASSERT_EQ(check(), FolderCheck::ok);
    // Same size and mtime: only the change time differs. A coarse kernel clock may stamp the
    // rewrite with the old ctime, so it is repeated until the record shows the change.
    const std::string old = slurp(cache());
    for (int i = 0; i < 100 && bb::common::game_check_record(eboot(), fake_hash()) == old; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        replace_keeping_mtime("NOT A REAL EBOOT");
    }
    ASSERT_NE(bb::common::game_check_record(eboot(), fake_hash()), old);
    EXPECT_EQ(check(), FolderCheck::unknown_eboot);
}

TEST_F(GameFolderCacheTest, AnotherExpectedDigestOrFolderMissesTheRecord) {
    make_game();
    const std::string record = bb::common::game_check_record(eboot(), fake_hash());
    EXPECT_NE(bb::common::game_check_record(eboot(), std::string(64, '0')), record);
    const fs::path copy = root_ / "copy";
    fs::copy(game_, copy, fs::copy_options::recursive);
    fs::last_write_time(copy / "eboot.bin", fs::last_write_time(eboot()));
    EXPECT_NE(bb::common::game_check_record(copy / "eboot.bin", fake_hash()), record);
    // A record for a symlinked path names the file it leads to.
    fs::create_directory_symlink(game_, root_ / "link");
    EXPECT_EQ(bb::common::game_check_record(root_ / "link/eboot.bin", fake_hash()), record);
}

TEST_F(GameFolderCacheTest, ACorruptCacheHashesAgainAndIsRewritten) {
    make_game();
    write(cache(), "eboot nonsense\n");
    EXPECT_EQ(check(), FolderCheck::ok);
    EXPECT_EQ(slurp(cache()), bb::common::game_check_record(eboot(), fake_hash()));
}

TEST_F(GameFolderCacheTest, ACacheFileThatCantBeWrittenOnlyCostsTheHash) {
    make_game();
    write(root_ / "cache", "a file where the folder should be");
    EXPECT_EQ(check(), FolderCheck::ok);
    EXPECT_EQ(check(), FolderCheck::ok);
}

TEST_F(GameFolderCacheTest, SetupUsesTheCache) {
    make_game();
    const fs::path config = root_ / "cfg/config.toml";
    ASSERT_TRUE(bb::common::setup_game_folder(game_, config, fake_hash(), cache()).ok());
    EXPECT_TRUE(fs::exists(cache()));
}

TEST(GameFolderReal, TheRealDumpPasses) {
    BB_REQUIRE_GAME(game);
    const auto r = bb::common::check_game_folder(game);
    EXPECT_TRUE(r.ok()) << r.message;
    EXPECT_EQ(r.app_version, "01.09");
    EXPECT_EQ(bb::common::check_game_folder(game.parent_path()).kind, FolderCheck::no_eboot);
}

}  // namespace
