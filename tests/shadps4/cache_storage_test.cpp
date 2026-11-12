// The pipeline cache's store (src/shadps4/video_core/cache_storage.cpp): what is saved reads back,
// and a file that is of another schema, truncated, damaged or half written reads as missing.

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include "video_core/cache_storage.h"

namespace {

namespace fs = std::filesystem;
using Storage::BlobType;
using Storage::DataBase;

class CacheStorage : public testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("bb-cache-storage-" + std::to_string(::getpid()) + "-" +
                testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(dir_);
    }
    void TearDown() override {
        DataBase::Instance().Close();
        fs::remove_all(dir_);
    }

    DataBase& open(u32 schema) {
        EXPECT_TRUE(DataBase::Instance().Open(dir_, schema));
        return DataBase::Instance();
    }
    void reopen(u32 schema) {
        DataBase::Instance().Close();
        open(schema);
    }
    std::vector<u8> load(const std::string& name, BlobType type = BlobType::ShaderMeta) {
        std::vector<u8> data;
        DataBase::Instance().Load(type, name, data);
        return data;
    }

    fs::path dir_;
};

TEST_F(CacheStorage, SavedBlobsReadBackAfterReopening) {
    auto& db = open(1);
    EXPECT_TRUE(db.Save(BlobType::ShaderMeta, "a", std::vector<u8>{1, 2, 3}));
    EXPECT_TRUE(db.Save(BlobType::ShaderBinary, "b", std::vector<u32>{0x07230203, 5, 6}));
    EXPECT_TRUE(db.Save(BlobType::ShaderMeta, "empty", std::vector<u8>{}));
    reopen(1); // Close writes what is queued
    EXPECT_EQ(load("a"), (std::vector<u8>{1, 2, 3}));
    std::vector<u32> words;
    DataBase::Instance().Load(BlobType::ShaderBinary, "b", words);
    EXPECT_EQ(words, (std::vector<u32>{0x07230203, 5, 6}));
    EXPECT_EQ(DataBase::Instance().DroppedBlobs(), 0u);
}

TEST_F(CacheStorage, AnotherSchemaReadsAsMissing) {
    auto& db = open(1);
    db.Save(BlobType::ShaderMeta, "a", std::vector<u8>{1, 2, 3});
    reopen(2);
    EXPECT_TRUE(load("a").empty());
    EXPECT_EQ(DataBase::Instance().DroppedBlobs(), 1u);
    // Rebuilt under the new schema, it reads back.
    DataBase::Instance().Save(BlobType::ShaderMeta, "a", std::vector<u8>{9});
    reopen(2);
    EXPECT_EQ(load("a"), (std::vector<u8>{9}));
}

TEST_F(CacheStorage, TruncatedAndDamagedFilesReadAsMissing) {
    auto& db = open(1);
    db.Save(BlobType::ShaderMeta, "cut", std::vector<u8>(100, 7));
    db.Save(BlobType::ShaderMeta, "flipped", std::vector<u8>(100, 7));
    db.Save(BlobType::ShaderMeta, "short", std::vector<u8>(100, 7));
    db.Save(BlobType::ShaderMeta, "garbage", std::vector<u8>(100, 7));
    db.Save(BlobType::ShaderMeta, "grown", std::vector<u8>(100, 7));
    db.Close();
    fs::resize_file(dir_ / "cut.meta", fs::file_size(dir_ / "cut.meta") - 1);
    fs::resize_file(dir_ / "short.meta", 5); // not even a header
    {
        std::fstream f{dir_ / "flipped.meta", std::ios::in | std::ios::out | std::ios::binary};
        f.seekp(-3, std::ios::end);
        f.put('\x55');
    }
    {
        std::ofstream f{dir_ / "garbage.meta", std::ios::binary | std::ios::trunc};
        f << std::string(300, 'x');
    }
    {
        std::ofstream f{dir_ / "grown.meta", std::ios::binary | std::ios::app};
        f << "tail";
    }
    open(1);
    for (const char* name : {"cut", "flipped", "short", "garbage", "grown"}) {
        EXPECT_TRUE(load(name).empty()) << name;
    }
    EXPECT_EQ(DataBase::Instance().DroppedBlobs(), 5u);
    EXPECT_TRUE(load("never-written").empty());
    EXPECT_EQ(DataBase::Instance().DroppedBlobs(), 5u); // a missing file is not a damaged one
}

TEST_F(CacheStorage, ForEachBlobSkipsDroppedFilesAndOtherTypes) {
    auto& db = open(1);
    db.Save(BlobType::PipelineKey, "k1", std::vector<u8>{1});
    db.Save(BlobType::PipelineKey, "k2", std::vector<u8>{2});
    db.Save(BlobType::PipelineKey, "k3", std::vector<u8>{3});
    db.Save(BlobType::ShaderMeta, "m", std::vector<u8>{4});
    db.Close();
    fs::resize_file(dir_ / "k2.key", 3);
    open(1);
    std::vector<u8> seen;
    DataBase::Instance().ForEachBlob(BlobType::PipelineKey,
                                     [&](std::vector<u8>&& d) { seen.push_back(d.at(0)); });
    std::ranges::sort(seen);
    EXPECT_EQ(seen, (std::vector<u8>{1, 3}));
}

// A run that died between creating the temporary file and renaming it leaves the old file in
// place and a stray temporary file, which the next Open removes.
TEST_F(CacheStorage, AKilledWriteLeavesTheOldFileAndNoTrustedTornOne) {
    auto& db = open(1);
    db.Save(BlobType::ShaderMeta, "a", std::vector<u8>{1, 2, 3});
    db.Close();
    {
        std::ofstream f{dir_ / "a.meta.tmp123-0", std::ios::binary};
        f << "half a file";
    }
    open(1);
    EXPECT_FALSE(fs::exists(dir_ / "a.meta.tmp123-0"));
    EXPECT_EQ(load("a"), (std::vector<u8>{1, 2, 3}));
}

TEST_F(CacheStorage, ClearDeletesBlobsOnly) {
    auto& db = open(1);
    db.Save(BlobType::ShaderMeta, "a", std::vector<u8>{1});
    db.Save(BlobType::DriverCache, "d", std::vector<u8>{2});
    db.Flush();
    {
        std::ofstream f{dir_ / "notes.txt"};
    }
    db.Clear();
    EXPECT_TRUE(load("a").empty());
    EXPECT_TRUE(load("d", BlobType::DriverCache).empty());
    EXPECT_TRUE(fs::exists(dir_ / "notes.txt"));
}

// Save runs on the thread that compiles; several may do so at once.
TEST_F(CacheStorage, SavesFromSeveralThreads) {
    auto& db = open(1);
    std::vector<std::thread> threads;
    for (u8 t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            for (u8 i = 0; i < 50; ++i) {
                db.Save(BlobType::ShaderBinary, "t" + std::to_string(t) + "_" + std::to_string(i),
                        std::vector<u8>(64, t));
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    db.Flush();
    u32 count = 0;
    db.ForEachBlob(BlobType::ShaderBinary, [&](std::vector<u8>&& d) {
        EXPECT_EQ(d.size(), 64u);
        ++count;
    });
    EXPECT_EQ(count, 200u);
}

TEST_F(CacheStorage, AnUnusableDirectoryLeavesTheStoreClosed) {
    fs::create_directories(dir_);
    std::ofstream{dir_ / "file"};
    EXPECT_FALSE(DataBase::Instance().Open(dir_ / "file" / "below", 1));
    EXPECT_FALSE(DataBase::Instance().IsOpened());
    EXPECT_FALSE(DataBase::Instance().Save(BlobType::ShaderMeta, "a", std::vector<u8>{1}));
    EXPECT_TRUE(load("a").empty());
}

} // namespace
