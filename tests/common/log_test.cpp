#include "common/log.hpp"

#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "common/runtime_dirs.hpp"

namespace {

namespace fs = std::filesystem;
using bb::common::LogFilter;
using bb::common::LogLevel;

TEST(LogFilter, DefaultsToInfo) {
    const LogFilter filter;
    EXPECT_EQ(filter.level("loader"), LogLevel::info);
    EXPECT_TRUE(filter.enabled(LogLevel::info, "loader"));
    EXPECT_FALSE(filter.enabled(LogLevel::debug, "loader"));
    EXPECT_EQ(LogFilter::parse("").level("hle.libkernel"), LogLevel::info);
}

TEST(LogFilter, ParsesChannelsAndDefault) {
    const LogFilter filter = LogFilter::parse("warn,hle=debug,hle.libkernel=trace,,game=off");
    EXPECT_EQ(filter.level("loader"), LogLevel::warn);
    EXPECT_EQ(filter.level("hle"), LogLevel::debug);
    EXPECT_EQ(filter.level("hle.libSceNet"), LogLevel::debug);
    EXPECT_EQ(filter.level("hle.libkernel"), LogLevel::trace);
    EXPECT_EQ(filter.level("hle.libkernel.x"), LogLevel::trace);
    // A prefix matches only at a dot.
    EXPECT_EQ(filter.level("hlex"), LogLevel::warn);
    EXPECT_EQ(filter.level("game"), LogLevel::off);
}

TEST(LogFilter, LaterEntryReplacesEarlier) {
    EXPECT_EQ(LogFilter::parse("loader=trace,loader=error").level("loader"), LogLevel::error);
    EXPECT_EQ(LogFilter::parse("trace,error").level("loader"), LogLevel::error);
}

TEST(LogFilter, RejectsBadEntries) {
    EXPECT_THROW(LogFilter::parse("loader=verbose"), std::invalid_argument);
    EXPECT_THROW(LogFilter::parse("loud"), std::invalid_argument);
    EXPECT_THROW(LogFilter::parse("=trace"), std::invalid_argument);
    EXPECT_THROW(LogFilter::parse("loader="), std::invalid_argument);
}

TEST(LogFilter, FiltersByLevelPerChannel) {
    const LogFilter filter = LogFilter::parse("error,hle=trace,game=off");
    EXPECT_TRUE(filter.enabled(LogLevel::trace, "hle.libkernel"));
    EXPECT_FALSE(filter.enabled(LogLevel::warn, "loader"));
    EXPECT_TRUE(filter.enabled(LogLevel::error, "loader"));
    EXPECT_FALSE(filter.enabled(LogLevel::error, "game"));
    // fatal always shows, `off` is no level to log at.
    EXPECT_TRUE(filter.enabled(LogLevel::fatal, "game"));
    EXPECT_FALSE(filter.enabled(LogLevel::off, "hle"));
}

TEST(LogLevel, NamesRoundTrip) {
    for (LogLevel level : {LogLevel::trace, LogLevel::debug, LogLevel::info, LogLevel::warn,
                           LogLevel::error, LogLevel::fatal, LogLevel::off}) {
        EXPECT_EQ(bb::common::parse_log_level(bb::common::log_level_name(level)), level);
    }
    EXPECT_EQ(bb::common::parse_log_level("Info"), std::nullopt);
}

// Tests that change the process's logger: XDG_STATE_HOME points into a fresh temporary directory
// (HOME too), and the filter, the log file and the environment are restored afterwards.
class LogTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::string templ = (fs::temp_directory_path() / "bb-log-test-XXXXXX").string();
        ASSERT_NE(::mkdtemp(templ.data()), nullptr);
        tmp_ = fs::canonical(templ);
        for (const char* key : {"HOME", "XDG_STATE_HOME", "BB_LOG"}) {
            const char* value = std::getenv(key);
            saved_[key] = value ? std::optional<std::string>(value) : std::nullopt;
        }
        ::setenv("HOME", (tmp_ / "home").c_str(), 1);
        ::setenv("XDG_STATE_HOME", (tmp_ / "state").c_str(), 1);
    }
    void TearDown() override {
        bb::common::close_log_file();
        bb::common::set_log_filter(LogFilter{});
        for (const auto& [key, value] : saved_) {
            if (value) {
                ::setenv(key.c_str(), value->c_str(), 1);
            } else {
                ::unsetenv(key.c_str());
            }
        }
        if (!tmp_.empty()) {
            fs::remove_all(tmp_);
        }
    }

    static std::string read(const fs::path& path) {
        std::ifstream in(path);
        std::stringstream s;
        s << in.rdbuf();
        return s.str();
    }

    fs::path tmp_;
    std::map<std::string, std::optional<std::string>> saved_;
};

TEST_F(LogTest, FileLandsInTheStateDirectory) {
    const auto dirs = bb::common::RuntimeDirs::from_environment("default", {});
    const fs::path path = bb::common::open_log_file(dirs);
    EXPECT_EQ(path.parent_path(), tmp_ / "state" / "bloodborne-linux" / "logs");
    EXPECT_TRUE(std::regex_match(path.filename().string(),
                                 std::regex("[0-9]{8}T[0-9]{6}Z-" + std::to_string(::getpid()) +
                                            "\\.log")))
        << path;
    struct stat st {};
    ASSERT_EQ(::stat(path.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0600U);

    testing::internal::CaptureStderr();
    bb::common::set_thread_name("log-test-main");
    bb::common::log(LogLevel::warn, "loader", "segment {:#x}", 0x400000);
    bb::common::log(LogLevel::debug, "loader", "hidden");
    bb::common::log_flush();
    const std::string err = testing::internal::GetCapturedStderr();

    const std::string file = read(path);
    EXPECT_EQ(file, err);
    EXPECT_TRUE(std::regex_match(
        file, std::regex("[0-9]{2}:[0-9]{2}:[0-9]{2}\\.[0-9]{3} warn \\[log-test-main\\] "
                         "loader: segment 0x400000\n")))
        << file;
    char host[16] = {};
    ASSERT_EQ(::pthread_getname_np(::pthread_self(), host, sizeof host), 0);
    EXPECT_STREQ(host, "log-test-main");

    bb::common::close_log_file();
    testing::internal::CaptureStderr();
    bb::common::log(LogLevel::error, "loader", "after close");
    EXPECT_NE(testing::internal::GetCapturedStderr().find("after close"), std::string::npos);
    EXPECT_EQ(read(path), file);
}

TEST_F(LogTest, FilterComesFromTheEnvironment) {
    ::setenv("BB_LOG", "error,hle=trace", 1);
    bb::common::set_log_filter_from_environment();
    EXPECT_TRUE(bb::common::log_enabled(LogLevel::trace, "hle.libkernel"));
    EXPECT_FALSE(bb::common::log_enabled(LogLevel::warn, "loader"));
    testing::internal::CaptureStderr();
    bb::common::log(LogLevel::warn, "loader", "hidden");
    bb::common::log(LogLevel::trace, "hle.libkernel", "shown");
    const std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(err.find("hidden"), std::string::npos) << err;
    EXPECT_NE(err.find(" trace ["), std::string::npos) << err;
    EXPECT_NE(err.find("] hle.libkernel: shown\n"), std::string::npos) << err;

    ::setenv("BB_LOG", "loader=chatty", 1);
    try {
        bb::common::set_log_filter_from_environment();
        FAIL() << "no exception";
    } catch (const std::invalid_argument& e) {
        EXPECT_NE(std::string(e.what()).find("BB_LOG"), std::string::npos) << e.what();
    }
    // The previous filter stays.
    EXPECT_TRUE(bb::common::log_enabled(LogLevel::trace, "hle.libkernel"));
}

TEST_F(LogTest, UnnamedThreadShowsItsId) {
    std::string name;
    std::thread([&] { name = bb::common::thread_name(); }).join();
    EXPECT_TRUE(std::regex_match(name, std::regex("[0-9]+"))) << name;
}

TEST_F(LogTest, LinesFromTwoThreadsNeverInterleave) {
    const auto dirs = bb::common::RuntimeDirs::from_environment("default", {});
    const fs::path path = bb::common::open_log_file(dirs);
    constexpr int lines = 2000;
    const std::string payload(300, 'x');
    auto writer = [&](const char* name) {
        bb::common::set_thread_name(name);
        for (int i = 0; i < lines; ++i) {
            bb::common::log(LogLevel::info, "game", "{} {}", i, payload);
        }
    };
    testing::internal::CaptureStderr();
    std::thread a(writer, "writer-a");
    std::thread b(writer, "writer-b");
    a.join();
    b.join();
    testing::internal::GetCapturedStderr();

    std::istringstream in(read(path));
    const std::regex line_re("[0-9:.]{12} info \\[(writer-[ab])\\] game: ([0-9]+) x{300}");
    std::map<std::string, int> next;
    std::string line;
    int count = 0;
    while (std::getline(in, line)) {
        std::smatch m;
        ASSERT_TRUE(std::regex_match(line, m, line_re)) << "line " << count << ": " << line;
        // Each thread's lines arrive whole and in order.
        EXPECT_EQ(std::stoi(m[2]), next[m[1]]++);
        ++count;
    }
    EXPECT_EQ(count, 2 * lines);
}

}  // namespace
