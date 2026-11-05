#include "common/log.hpp"

#include <cstdlib>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>

#include <gtest/gtest.h>

namespace {

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

// Tests that change the process's logger: the filter and BB_LOG are restored afterwards.
class LogTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* value = std::getenv("BB_LOG");
        saved_ = value ? std::optional<std::string>(value) : std::nullopt;
    }
    void TearDown() override {
        bb::common::set_log_filter(LogFilter{});
        if (saved_) {
            ::setenv("BB_LOG", saved_->c_str(), 1);
        } else {
            ::unsetenv("BB_LOG");
        }
    }

    std::optional<std::string> saved_;
};

TEST_F(LogTest, WritesLevelThreadAndChannel) {
    testing::internal::CaptureStderr();
    bb::common::set_thread_name("log-test-main");
    bb::common::log(LogLevel::warn, "loader", "segment {:#x}", 0x400000);
    bb::common::log(LogLevel::debug, "loader", "hidden");
    bb::common::log_flush();
    const std::string err = testing::internal::GetCapturedStderr();
    EXPECT_TRUE(std::regex_match(
        err, std::regex("[0-9]{2}:[0-9]{2}:[0-9]{2}\\.[0-9]{3} warn \\[log-test-main\\] "
                        "loader: segment 0x400000\n")))
        << err;
    char host[16] = {};
    ASSERT_EQ(::pthread_getname_np(::pthread_self(), host, sizeof host), 0);
    EXPECT_STREQ(host, "log-test-main");
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

    std::istringstream in(testing::internal::GetCapturedStderr());
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
