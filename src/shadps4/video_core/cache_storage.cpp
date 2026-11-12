// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Source: shadPS4 src/video_core/cache_storage.cpp @11557f2eb60b3b00712c8887c30852de7615d6df
// Changed for bloodborne-linux, 2026-11-13
// License texts: third_party/shadps4/LICENSES/

#include "common/assert.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/thread.h"

#include "video_core/cache_storage.h"

#include <unistd.h>

#include <cstring>
#include <format>
#include <fstream>
#include <system_error>

namespace Storage {

bool pipeline_cache_enabled = true;

namespace {

constexpr u32 BlobMagic = 0x43534242; // "BBSC"

// The header of every blob file. The checksum covers the payload; a file cut short or changed
// fails the size or the checksum test.
struct BlobHeader {
    u32 magic;
    u32 schema;
    u64 payload_size;
    u64 checksum;
};

// FNV-1a over 64-bit words, the tail byte by byte. It detects tearing and bit rot, not tampering.
u64 Checksum(const u8* data, size_t size) {
    constexpr u64 prime = 0x100000001b3ull;
    u64 hash = 0xcbf29ce484222325ull;
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        u64 word;
        std::memcpy(&word, data + i, 8);
        hash = (hash ^ word) * prime;
        hash ^= hash >> 29;
    }
    for (; i < size; ++i) {
        hash = (hash ^ data[i]) * prime;
    }
    return hash ^ size;
}

constexpr std::string GetBlobFileExtension(BlobType type) {
    switch (type) {
    case BlobType::ShaderMeta:
        return "meta";
    case BlobType::ShaderBinary:
        return "spv";
    case BlobType::PipelineKey:
        return "key";
    case BlobType::ShaderProfile:
        return "bin";
    case BlobType::DriverCache:
        return "vkc";
    default:
        UNREACHABLE();
    }
}

std::filesystem::path BlobPath(const std::filesystem::path& dir, BlobType type,
                               const std::string& name) {
    std::filesystem::path path = dir / name;
    path.replace_extension(GetBlobFileExtension(type));
    return path;
}

// Writes header and payload to a temporary file next to `path` and renames it over `path`, so a
// reader never sees a partial file and a killed run leaves at most a stray temporary file.
bool WriteAtomic(const std::filesystem::path& path, u32 schema, const std::vector<u8>& payload) {
    static std::atomic<u32> counter{};
    auto tmp = path;
    tmp += std::format(".tmp{}-{}", ::getpid(), counter++);
    const BlobHeader header{BlobMagic, schema, payload.size(),
                            Checksum(payload.data(), payload.size())};
    bool ok = false;
    {
        std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        out.write(reinterpret_cast<const char*>(payload.data()),
                  static_cast<std::streamsize>(payload.size()));
        out.flush();
        ok = static_cast<bool>(out);
    }
    std::error_code ec;
    if (ok) {
        std::filesystem::rename(tmp, path, ec);
    }
    if (!ok || ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace

bool DataBase::Open(u32 schema_) {
    const auto& game_info = Common::ElfInfo::Instance();
    return Open(Common::FS::GetUserPath(Common::FS::PathType::CacheDir) / game_info.GameSerial(),
                schema_);
}

bool DataBase::Open(const std::filesystem::path& dir, u32 schema_) {
    if (opened) {
        return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec || !std::filesystem::is_directory(dir, ec)) {
        LOG_WARNING(Render, "Cannot use {} for the pipeline cache, running without it",
                    dir.string());
        return false;
    }
    // Temporary files of runs that were killed in the middle of a write.
    for (const auto& entry : std::filesystem::directory_iterator{dir, ec}) {
        if (entry.path().extension().string().starts_with(".tmp")) {
            std::filesystem::remove(entry.path(), ec);
        }
    }
    cache_path = dir;
    schema = schema_;
    dropped = 0;
    write_failed = false;
    stopping = false;
    io_worker = std::thread{[this] { Worker(); }};
    opened = true;
    return true;
}

void DataBase::Worker() {
    Common::SetCurrentThreadName("shadPS4:PipelineCacheIO");
    std::unique_lock lock{queue_mutex};
    while (true) {
        queue_cv.wait(lock, [&] { return !queue.empty() || stopping; });
        if (queue.empty()) {
            return; // stopping, and everything queued is written
        }
        auto task = std::move(queue.front());
        queue.pop_front();
        busy = true;
        lock.unlock();
        task();
        lock.lock();
        busy = false;
        idle_cv.notify_all();
    }
}

void DataBase::Flush() {
    if (!opened) {
        return;
    }
    std::unique_lock lock{queue_mutex};
    idle_cv.wait(lock, [&] { return queue.empty() && !busy; });
}

void DataBase::Close() {
    if (!opened) {
        return;
    }
    {
        std::scoped_lock lock{queue_mutex};
        stopping = true;
    }
    queue_cv.notify_all();
    io_worker.join();
    opened = false;
}

void DataBase::Clear() {
    if (!opened) {
        return;
    }
    Flush();
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator{cache_path, ec}) {
        for (const char* ext : {".meta", ".spv", ".key", ".bin", ".vkc"}) {
            if (entry.path().extension() == ext) {
                std::filesystem::remove(entry.path(), ec);
            }
        }
    }
}

bool DataBase::Enqueue(BlobType type, const std::string& name, std::vector<u8>&& payload) {
    if (!opened) {
        return false;
    }
    auto path = BlobPath(cache_path, type, name);
    auto task = [this, path = std::move(path), payload = std::move(payload)] {
        if (!WriteAtomic(path, schema, payload) && !write_failed.exchange(true)) {
            LOG_WARNING(Render, "Cannot write the pipeline cache file {}", path.string());
        }
    };
    {
        std::scoped_lock lock{queue_mutex};
        queue.emplace_back(std::move(task));
    }
    queue_cv.notify_one();
    return true;
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u8>&& data) {
    return Enqueue(type, name, std::move(data));
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u32>&& data) {
    std::vector<u8> bytes(data.size() * sizeof(u32));
    std::memcpy(bytes.data(), data.data(), bytes.size());
    return Enqueue(type, name, std::move(bytes));
}

// Reads one file. False, with the file counted as dropped, when it fails a test; a file that does
// not exist is not counted.
bool DataBase::ReadBlob(const std::filesystem::path& file, std::vector<u8>& payload) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        return false;
    }
    std::ifstream in{file, std::ios::binary};
    BlobHeader header{};
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    bool ok = in.gcount() == sizeof(header) && header.magic == BlobMagic &&
              header.schema == schema;
    if (ok) {
        const auto file_size = std::filesystem::file_size(file, ec);
        ok = !ec && file_size == sizeof(header) + header.payload_size;
    }
    if (ok) {
        payload.resize(header.payload_size);
        in.read(reinterpret_cast<char*>(payload.data()),
                static_cast<std::streamsize>(payload.size()));
        ok = static_cast<u64>(in.gcount()) == header.payload_size &&
             Checksum(payload.data(), payload.size()) == header.checksum;
    }
    if (!ok) {
        payload.clear();
        ++dropped;
    }
    return ok;
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u8>& data) {
    data.clear();
    if (!opened) {
        return;
    }
    std::vector<u8> payload;
    if (ReadBlob(BlobPath(cache_path, type, name), payload)) {
        data = std::move(payload);
    }
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u32>& data) {
    data.clear();
    if (!opened) {
        return;
    }
    std::vector<u8> payload;
    if (ReadBlob(BlobPath(cache_path, type, name), payload) && payload.size() % sizeof(u32) == 0) {
        data.resize(payload.size() / sizeof(u32));
        std::memcpy(data.data(), payload.data(), payload.size());
    }
}

void DataBase::ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func) {
    if (!opened) {
        return;
    }
    const auto ext = "." + GetBlobFileExtension(type);
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator{cache_path, ec}) {
        if (entry.path().extension() == ext) {
            files.push_back(entry.path());
        }
    }
    for (const auto& file : files) {
        std::vector<u8> payload;
        if (ReadBlob(file, payload)) {
            func(std::move(payload));
        }
    }
}

} // namespace Storage
