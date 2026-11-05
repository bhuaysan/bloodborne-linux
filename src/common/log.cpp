#include "common/log.hpp"

#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <system_error>

#include "common/runtime_dirs.hpp"

namespace bb::common {
namespace fs = std::filesystem;
namespace {

constexpr std::array<std::string_view, 7> level_names = {"trace", "debug", "info", "warn",
                                                         "error", "fatal", "off"};

struct Logger {
    std::mutex mutex;  // serialises whole lines and guards file_fd and filters
    int file_fd = -1;
    // Every filter ever set, kept because a thread may still be reading a replaced one.
    // set_log_filter is rare.
    std::vector<std::unique_ptr<const LogFilter>> filters;
    std::atomic<const LogFilter*> filter{filters.emplace_back(new LogFilter).get()};
};

// Leaked on purpose: threads and atexit handlers may log while the process exits.
Logger& logger() {
    static Logger& l = *new Logger;
    return l;
}

thread_local std::string current_thread_name;

void write_all(int fd, std::string_view data) {
    while (!data.empty()) {
        const ssize_t n = ::write(fd, data.data(), data.size());
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return;  // nowhere left to report it
        }
        data.remove_prefix(static_cast<std::size_t>(n));
    }
}

// "hh:mm:ss.mmm" of now, in UTC.
std::string clock_text() {
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    tm utc{};
    ::gmtime_r(&ts.tv_sec, &utc);
    return std::format("{:02}:{:02}:{:02}.{:03}", utc.tm_hour, utc.tm_min, utc.tm_sec,
                       ts.tv_nsec / 1'000'000);
}

}  // namespace

std::string_view log_level_name(LogLevel level) {
    return level_names.at(static_cast<std::size_t>(level));
}

std::optional<LogLevel> parse_log_level(std::string_view name) {
    const auto it = std::ranges::find(level_names, name);
    if (it == level_names.end()) {
        return std::nullopt;
    }
    return static_cast<LogLevel>(it - level_names.begin());
}

LogFilter LogFilter::parse(std::string_view spec) {
    LogFilter filter;
    while (!spec.empty()) {
        const std::size_t comma = spec.find(',');
        const std::string_view entry = spec.substr(0, comma);
        spec = comma == std::string_view::npos ? std::string_view{} : spec.substr(comma + 1);
        if (entry.empty()) {
            continue;
        }
        const std::size_t eq = entry.find('=');
        const std::string_view name = eq == std::string_view::npos ? entry : entry.substr(eq + 1);
        const std::optional<LogLevel> level = parse_log_level(name);
        if (!level) {
            throw std::invalid_argument(std::format("unknown log level '{}' in '{}'", name, entry));
        }
        if (eq == std::string_view::npos) {
            filter.default_ = *level;
            continue;
        }
        const std::string_view channel = entry.substr(0, eq);
        if (channel.empty()) {
            throw std::invalid_argument(std::format("empty channel name in '{}'", entry));
        }
        std::erase_if(filter.channels_, [&](const auto& c) { return c.first == channel; });
        filter.channels_.emplace_back(channel, *level);
    }
    return filter;
}

LogLevel LogFilter::level(std::string_view channel) const {
    LogLevel level = default_;
    std::size_t best = 0;
    for (const auto& [name, l] : channels_) {
        const bool matches =
            channel.starts_with(name) && (channel.size() == name.size() || channel[name.size()] == '.');
        if (matches && name.size() >= best) {
            best = name.size();
            level = l;
        }
    }
    return level;
}

void set_log_filter(LogFilter filter) {
    Logger& l = logger();
    const std::lock_guard lock(l.mutex);
    l.filter.store(l.filters.emplace_back(new LogFilter(std::move(filter))).get());
}

void set_log_filter_from_environment() {
    const char* spec = std::getenv("BB_LOG");
    try {
        set_log_filter(LogFilter::parse(spec ? spec : ""));
    } catch (const std::invalid_argument& e) {
        throw std::invalid_argument(std::string("BB_LOG: ") + e.what());
    }
}

bool log_enabled(LogLevel level, std::string_view channel) {
    return logger().filter.load()->enabled(level, channel);
}

void log_message(LogLevel level, std::string_view channel, std::string_view message) {
    if (!log_enabled(level, channel)) {
        return;
    }
    std::string line = std::format("{} {} [{}] {}: {}\n", clock_text(), log_level_name(level),
                                   thread_name(), channel, message);
    Logger& l = logger();
    const std::lock_guard lock(l.mutex);
    write_all(STDERR_FILENO, line);
    if (l.file_fd >= 0) {
        write_all(l.file_fd, line);
    }
}

void log_fatal(std::string_view channel, std::string_view message) {
    log_message(LogLevel::fatal, channel, message);
    log_flush();
    std::abort();
}

fs::path open_log_file(const RuntimeDirs& dirs) {
    const fs::path dir = dirs.ensure(dirs.logs());
    const std::time_t now = std::time(nullptr);
    tm utc{};
    ::gmtime_r(&now, &utc);
    const fs::path path =
        dir / std::format("{:04}{:02}{:02}T{:02}{:02}{:02}Z-{}.log", utc.tm_year + 1900,
                          utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec,
                          ::getpid());
    const int fd =
        ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        throw fs::filesystem_error("cannot create log file", path,
                                   std::error_code(errno, std::generic_category()));
    }
    Logger& l = logger();
    const std::lock_guard lock(l.mutex);
    if (l.file_fd >= 0) {
        ::close(l.file_fd);
    }
    l.file_fd = fd;
    return path;
}

void close_log_file() {
    Logger& l = logger();
    const std::lock_guard lock(l.mutex);
    if (l.file_fd >= 0) {
        ::close(l.file_fd);
        l.file_fd = -1;
    }
}

void log_flush() {
    Logger& l = logger();
    const std::lock_guard lock(l.mutex);
    if (l.file_fd >= 0) {
        ::fdatasync(l.file_fd);
    }
}

void set_thread_name(std::string_view name) {
    current_thread_name = name;
    const std::string host(name.substr(0, 15));
    ::pthread_setname_np(::pthread_self(), host.c_str());
}

std::string thread_name() {
    return current_thread_name.empty() ? std::to_string(::gettid()) : current_thread_name;
}

}  // namespace bb::common
