// The functions src/shadps4 expects from the rest of the emulator.

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <string>

#include <pthread.h>

#include <fmt/format.h>

#include "common/assert.h"
#include "common/io_file.h"
#include "common/log.hpp"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/runtime_dirs.hpp"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "shadps4_host/failure.hpp"

namespace {

// The last Critical line of this thread: what a failing assertion logged just before it calls
// assert_fail_impl.
thread_local std::string last_critical;

bb::common::LogLevel to_bb(Common::Log::Level level) {
    using enum Common::Log::Level;
    switch (level) {
    case Trace:
        return bb::common::LogLevel::trace;
    case Debug:
        return bb::common::LogLevel::debug;
    case Info:
        return bb::common::LogLevel::info;
    case Warning:
        return bb::common::LogLevel::warn;
    default:  // Error and Critical. A failed assertion is the caller's to judge, so not fatal.
        return bb::common::LogLevel::error;
    }
}

}  // namespace

namespace Common::Log {

std::array<Level, NUM_LOG_CLASSES> g_class_levels = [] {
    std::array<Level, NUM_LOG_CLASSES> levels{};
    levels.fill(Level::Warning);
    return levels;
}();

void VLog(Class log_class, Level level, const char* file, int line, const char* func,
          fmt::string_view format, fmt::format_args args) {
    std::string message = fmt::vformat(format, args);
    message.erase(std::remove(message.begin(), message.end(), '\n'), message.end());
    const std::string text = fmt::format("{}:{} {}: {}", file, line, func, message);
    if (level == Level::Critical) {
        last_critical = text;
    }
    bb::common::log_message(to_bb(level), NameOf(log_class), text);
}

}  // namespace Common::Log

void assert_fail_impl() {
    throw bb::shader::TranslationFailure(last_critical);
}

[[noreturn]] void unreachable_impl() {
    throw bb::shader::TranslationFailure(last_critical);
}

[[noreturn]] void bb::shadps4::host_missing(std::string_view what) {
    bb::common::log_fatal("shadps4", fmt::format("not provided by the host layer: {}", what));
}

namespace {
std::atomic<s32> chosen_gpu_id{-1};
}  // namespace

s32 bb::shader::gpu_id() { return chosen_gpu_id; }
void bb::shader::set_gpu_id(s32 id) { chosen_gpu_id = id; }

// Names the calling thread for debuggers. Linux takes at most 15 characters.
void Common::SetCurrentThreadName(const char* name) {
    pthread_setname_np(pthread_self(), std::string(name).substr(0, 15).c_str());
}

namespace Common::FS {

// shadPS4's user directories, inside the runtime's XDG directories (bb::common::RuntimeDirs).
// Nothing is created here. Types the port never asks for end the process.
const std::filesystem::path& GetUserPath(PathType user_path) {
    static const std::map<PathType, std::filesystem::path> paths = [] {
        const auto dirs =
            bb::common::RuntimeDirs::from_environment(bb::common::default_profile, {});
        return std::map<PathType, std::filesystem::path>{
            {PathType::UserDir, dirs.data()},
            {PathType::LogDir, dirs.logs()},
            {PathType::ScreenshotsDir, dirs.data() / "screenshots"},
            {PathType::CacheDir, dirs.cache() / "pipeline"},
            {PathType::CapturesDir, dirs.data() / "captures"},
        };
    }();
    const auto it = paths.find(user_path);
    if (it == paths.end()) {
        bb::shadps4::host_missing(
            fmt::format("Common::FS::GetUserPath({})", static_cast<int>(user_path)));
    }
    return it->second;
}

std::string GetUserPathString(PathType user_path) {
    return GetUserPath(user_path).string();
}

// Files are written only by screenshots and read only by the pipeline cache and PNG icons.
// The port builds none of them.
IOFile::IOFile(const std::filesystem::path&, FileAccessMode, FileType, FileShareFlag) {
    bb::shadps4::host_missing("Common::FS::IOFile");
}

IOFile::~IOFile() = default;

// No IOFile can be opened, so these are not reached either.
u64 IOFile::GetSize() const { bb::shadps4::host_missing("Common::FS::IOFile::GetSize"); }
bool IOFile::Seek(s64, SeekOrigin) const { bb::shadps4::host_missing("Common::FS::IOFile::Seek"); }
void IOFile::Close() { bb::shadps4::host_missing("Common::FS::IOFile::Close"); }

}  // namespace Common::FS
