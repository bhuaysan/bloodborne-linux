// bb-run's command line and entry arguments (src/tools/run_options.hpp).

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "tools/run_options.hpp"

namespace {

using bb::tools::parse_run_options;
using bb::tools::RunOptions;

RunOptions parse(std::vector<std::string_view> args) {
    return parse_run_options(args);
}

// The message of the std::invalid_argument that parsing `args` throws.
std::string parse_error(std::vector<std::string_view> args) {
    try {
        parse(args);
    } catch (const std::invalid_argument& e) {
        return e.what();
    }
    return "no error";
}

TEST(RunOptions, Defaults) {
    const RunOptions o = parse({});
    EXPECT_FALSE(o.game_dir);
    EXPECT_EQ(o.profile, "default");
    EXPECT_FALSE(o.timeout_seconds);
    EXPECT_FALSE(o.stop_at_main_loop);
    EXPECT_FALSE(o.log_spec);
    EXPECT_TRUE(o.log_file);
    EXPECT_FALSE(o.help);
}

TEST(RunOptions, ParsesEveryOption) {
    const RunOptions o = parse({"--game-dir", "/games/bb", "--profile", "test_1-a", "--timeout",
                                "10", "--stop-at-main-loop", "--log", "hle=trace,warn",
                                "--no-log-file"});
    EXPECT_EQ(o.game_dir, std::filesystem::path("/games/bb"));
    EXPECT_EQ(o.profile, "test_1-a");
    EXPECT_EQ(o.timeout_seconds, 10U);
    EXPECT_TRUE(o.stop_at_main_loop);
    EXPECT_EQ(o.log_spec, "hle=trace,warn");
    EXPECT_FALSE(o.log_file);
    EXPECT_TRUE(parse({"-h"}).help);
    EXPECT_TRUE(parse({"--help"}).help);
    // A later value replaces an earlier one.
    EXPECT_EQ(parse({"--timeout", "1", "--timeout", "86400"}).timeout_seconds, 86400U);
}

TEST(RunOptions, RefusesBadArguments) {
    EXPECT_EQ(parse_error({"--frobnicate"}), "unknown argument \"--frobnicate\"");
    EXPECT_EQ(parse_error({"eboot.bin"}), "unknown argument \"eboot.bin\"");
    EXPECT_EQ(parse_error({"--profile"}), "--profile needs a value");
    EXPECT_EQ(parse_error({"--game-dir"}), "--game-dir needs a value");
    EXPECT_EQ(parse_error({"--log"}), "--log needs a value");
    EXPECT_EQ(parse_error({"--timeout"}), "--timeout needs a value");
    for (std::string_view name :
         {"", "a/b", "../x", "with space", "abcdefghijklmnopqrstuvwxyz0123456"}) {
        EXPECT_TRUE(parse_error({"--profile", name}).starts_with("--profile: ")) << name;
    }
    for (std::string_view t : {"0", "86401", "-1", "10s", "", "1.5", " 1", "99999999999"}) {
        EXPECT_TRUE(parse_error({"--timeout", t}).starts_with("--timeout: ")) << t;
    }
    EXPECT_TRUE(parse_error({"--log", "hle=loud"}).starts_with("--log: "))
        << parse_error({"--log", "hle=loud"});
}

TEST(EntryArgs, MatchesTheEntrysReads) {
    const bb::tools::EntryArgs args = bb::tools::entry_args();
    // The entry reads a 4-byte argc at +0 and takes argv as the address +8.
    const auto* bytes = reinterpret_cast<const unsigned char*>(&args);
    std::int32_t argc = 0;
    std::memcpy(&argc, bytes, sizeof argc);
    EXPECT_EQ(argc, 1);
    const char* const* argv = reinterpret_cast<const char* const*>(bytes + 8);
    EXPECT_STREQ(argv[0], "/app0/eboot.bin");
    EXPECT_EQ(argv[1], nullptr);
    EXPECT_EQ(sizeof args, 24U);
    bb::tools::entry_exit_handler();  // does nothing, and returns
}

}  // namespace
