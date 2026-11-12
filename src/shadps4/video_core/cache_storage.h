// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/video_core/cache_storage.h @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-13
// License texts: third_party/shadps4/LICENSES/

#pragma once

#include "common/path_util.h"
#include "common/singleton.h"
#include "common/types.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace Storage {

// Whether the renderer keeps its shader and pipeline cache on disk (bb-run --no-pipeline-cache
// turns it off for comparison runs). Set before the renderer is created.
extern bool pipeline_cache_enabled;

enum class BlobType : u32 {
    ShaderMeta,
    ShaderBinary,
    PipelineKey,
    ShaderProfile,
    DriverCache, // the VkPipelineCache's data
};

// The cache directory: one file per blob, named <name>.<extension of its type>. Every file has a
// header with a magic number, the schema, the payload size and a checksum, and is written to a
// temporary file that is renamed over the old one. A file that is torn, corrupt or of another
// schema reads as missing, never as an error. Save, Load and ForEachBlob may be called from any
// thread; Open and Close are not thread safe. A standalone tool opens the directory with
// Open(dir, schema) and the schema of the renderer's own cache.
class DataBase {
public:
    static DataBase& Instance() {
        return *Common::Singleton<DataBase>::Instance();
    }

    // The runtime's cache directory (runtime_dirs::cache()) below the game's serial.
    bool Open(u32 schema);
    // `dir` is created when missing. Returns false, leaving the store closed, when it cannot be.
    bool Open(const std::filesystem::path& dir, u32 schema);
    // Writes what is queued, then closes.
    void Close();
    [[nodiscard]] bool IsOpened() const {
        return opened;
    }
    // Returns once every queued write is on disk.
    void Flush();
    // Deletes every blob file of the directory.
    void Clear();
    void FinishPreload() {}

    // Queues the write. False when the store is closed.
    bool Save(BlobType type, const std::string& name, std::vector<u8>&& data);
    bool Save(BlobType type, const std::string& name, std::vector<u32>&& data);

    // `data` is empty when the blob is missing or dropped.
    void Load(BlobType type, const std::string& name, std::vector<u8>& data);
    void Load(BlobType type, const std::string& name, std::vector<u32>& data);

    void ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func);

    // Files read since Open that were dropped: torn, corrupt or of another schema.
    [[nodiscard]] u32 DroppedBlobs() const {
        return dropped;
    }

private:
    void Worker();
    bool Enqueue(BlobType type, const std::string& name, std::vector<u8>&& payload);
    bool ReadBlob(const std::filesystem::path& file, std::vector<u8>& payload);

    std::thread io_worker{};
    std::mutex queue_mutex{};
    std::condition_variable queue_cv{};
    std::condition_variable idle_cv{};
    std::deque<std::function<void()>> queue{};
    bool busy{};
    bool stopping{};
    std::filesystem::path cache_path{};
    u32 schema{};
    std::atomic<u32> dropped{};
    std::atomic<bool> write_failed{};
    bool opened{};
};

} // namespace Storage
