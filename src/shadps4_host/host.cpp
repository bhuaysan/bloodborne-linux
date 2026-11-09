// The functions src/shadps4 expects from the rest of the emulator.

#include <algorithm>
#include <array>
#include <string>

#include <fmt/format.h>

#include "common/assert.h"
#include "common/io_file.h"
#include "common/log.hpp"
#include "common/logging/log.h"
#include "common/path_util.h"
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

// Only the shader dumps use these, and EmulatorSettings keeps them off.
namespace Common::FS {

const std::filesystem::path& GetUserPath(PathType) {
    UNREACHABLE_MSG("shader dumps are disabled");
}

IOFile::IOFile(const std::filesystem::path&, FileAccessMode, FileType, FileShareFlag) {
    UNREACHABLE_MSG("shader dumps are disabled");
}

IOFile::~IOFile() = default;

}  // namespace Common::FS
