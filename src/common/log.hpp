// The runtime's logger: levels, channels, the thread name on every line, output to stderr and,
// once open_log_file was called, to a log file (docs/design/m3-core-os.md, "Logging").
//
// Channels are dotted names: "loader", "hle.<library>", "game". Every line is written with one
// write(2) per output under one lock, so lines from different threads never interleave and
// nothing is buffered in the process: a line that log() returned from is in the kernel, also
// after _exit or abort. Not async-signal-safe.
#pragma once

#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/runtime_dirs.hpp"

namespace bb::common {

// In increasing severity. A channel at level L shows L and everything above it. `fatal` is
// always shown; `off` shows nothing else.
enum class LogLevel { trace, debug, info, warn, error, fatal, off };

// "trace", "debug", "info", "warn", "error", "fatal" or "off".
std::string_view log_level_name(LogLevel level);
std::optional<LogLevel> parse_log_level(std::string_view name);

// Which level each channel logs at.
class LogFilter {
public:
    // Everything at info and above.
    LogFilter() = default;

    // Parses a BB_LOG value: comma-separated entries, each `<channel>=<level>` or a bare
    // `<level>` that sets the default. An entry for "hle" also covers "hle.libkernel"; the
    // longest matching entry wins, and a later entry for the same channel replaces an earlier
    // one. Spaces and tabs around entries, channels and levels are ignored, and so are empty
    // entries. Throws std::invalid_argument for an unknown level or an
    // empty channel name.
    static LogFilter parse(std::string_view spec);

    LogLevel level(std::string_view channel) const;
    bool enabled(LogLevel level, std::string_view channel) const {
        return level == LogLevel::fatal || (level >= this->level(channel) && level != LogLevel::off);
    }

private:
    LogLevel default_ = LogLevel::info;
    std::vector<std::pair<std::string, LogLevel>> channels_;
};

// Replaces the process's filter. Safe to call while other threads log.
void set_log_filter(LogFilter filter);
// Sets the filter from BB_LOG (unset or empty: the default). Throws as LogFilter::parse, with the
// variable's name in the message.
void set_log_filter_from_environment();

// Whether a line at `level` on `channel` would be written. Cheap enough for every call.
bool log_enabled(LogLevel level, std::string_view channel);

// Writes one line, `<UTC hh:mm:ss.mmm> <level> [<thread>] <channel>: <message>`, to stderr and
// the log file if the filter lets it through. A newline in `message` stays part of the line.
void log_message(LogLevel level, std::string_view channel, std::string_view message);

// Formats only when the line is written.
template <class... Args>
void log(LogLevel level, std::string_view channel, std::format_string<Args...> format,
         Args&&... args) {
    if (log_enabled(level, channel)) {
        log_message(level, channel, std::format(format, std::forward<Args>(args)...));
    }
}

// Logs at fatal level, flushes and aborts.
[[noreturn]] void log_fatal(std::string_view channel, std::string_view message);

// Waits for a line that another thread is writing and syncs the log file. Call it before _exit
// and abort.
void log_flush();

// Creates `dirs.logs()` (RuntimeDirs::ensure) and in it `<UTC yyyymmddThhmmssZ>-<pid>.log`
// (mode 0600, never an existing file), writes every later line there as well, and returns its
// path. bb-run (#101) calls it once at start. A file opened before is closed. Throws
// std::filesystem::filesystem_error when the directory or the file cannot be created.
std::filesystem::path open_log_file(const RuntimeDirs& dirs);
// Closes the log file, if one is open. Later lines go to stderr only.
void close_log_file();

// Names the calling thread in the log and, cut to 15 bytes, for the host (pthread_setname_np).
void set_thread_name(std::string_view name);
// The name set_thread_name gave the calling thread, else its host thread id as a number.
std::string thread_name();

}  // namespace bb::common
