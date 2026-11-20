#include "common/config.hpp"

#include <unistd.h>

#include <cstdlib>
#include <format>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

namespace {

namespace fs = std::filesystem;
using bb::common::Config;
using bb::common::ConfigError;
using bb::common::parse_config;

// The message of the ConfigError that parsing `text` throws.
std::string error_of(const std::string& text) {
    try {
        parse_config(text, "c.toml");
    } catch (const ConfigError& e) {
        return e.what();
    }
    return "no error";
}

TEST(Config, EmptyTextSetsNothing) {
    EXPECT_EQ(parse_config("", "c").config, Config{});
    EXPECT_EQ(parse_config("# only a comment\n\n  \n", "c").config, Config{});
}

TEST(Config, ParsesEverySetting) {
    const auto parsed = parse_config(
        "# settings\n"
        "game_dir = \"/games/bb\"  # the dump\n"
        "profile = \"main\"\n"
        "window_size = \"2560x1440\"\n"
        "fullscreen = true\n"
        "fsr = true\r\n"
        "rcas = false\n"
        "rcas_attenuation = 0.5\n"
        "frame_rate = 60\n"
        "anisotropy = 16\n"
        "motion_blur = false\n"
        "motion_blur_strength = 0.5\n"
        "chromatic_aberration = true\n"
        "chromatic_aberration_strength = 0.25\n"
        "fov = 55.5",
        "c");
    const Config& c = parsed.config;
    EXPECT_EQ(c.game_dir, fs::path("/games/bb"));
    EXPECT_EQ(c.profile, "main");
    ASSERT_TRUE(c.window_size);
    EXPECT_EQ(c.window_size->first, 2560u);
    EXPECT_EQ(c.window_size->second, 1440u);
    EXPECT_EQ(c.fullscreen, true);
    EXPECT_EQ(c.fsr, true);
    EXPECT_EQ(c.rcas, false);
    EXPECT_EQ(c.rcas_attenuation, 0.5f);
    EXPECT_EQ(c.frame_rate, 60u);
    EXPECT_EQ(c.anisotropy, 16u);
    EXPECT_EQ(c.motion_blur, false);
    EXPECT_EQ(c.motion_blur_strength, 0.5f);
    EXPECT_EQ(c.chromatic_aberration, true);
    EXPECT_EQ(c.chromatic_aberration_strength, 0.25f);
    EXPECT_EQ(c.fov, 55.5f);
    EXPECT_TRUE(parsed.unknown_keys.empty());
}

TEST(Config, RefusesPostEffectStrengthsOutOfRange) {
    for (const char* key : {"motion_blur_strength", "chromatic_aberration_strength"}) {
        for (const char* value : {"-0.1", "1.01", "nan", "\"0.5\"", "x"}) {
            try {
                parse_config(std::format("{} = {}", key, value), "c.toml");
                ADD_FAILURE() << key << " = " << value;
            } catch (const bb::common::ConfigError& e) {
                EXPECT_NE(std::string(e.what()).find(std::format("c.toml:1: {}: ", key)),
                          std::string::npos)
                    << e.what();
                EXPECT_NE(std::string(e.what()).find("is not a number from 0 to 1"),
                          std::string::npos)
                    << e.what();
            }
        }
        EXPECT_EQ(parse_config(std::format("{} = 0", key), "c").unknown_keys.size(), 0u);
    }
}

TEST(Config, RefusesAFieldOfViewOutOfRange) {
    for (const char* value : {"29.9", "65.5", "0", "nan", "\"60\"", "x"}) {
        try {
            parse_config(std::format("fov = {}", value), "c.toml");
            ADD_FAILURE() << value;
        } catch (const bb::common::ConfigError& e) {
            EXPECT_NE(std::string(e.what()).find("c.toml:1: fov: "), std::string::npos) << e.what();
            EXPECT_NE(std::string(e.what()).find("is not a number from 30 to 65"),
                      std::string::npos)
                << e.what();
        }
    }
    EXPECT_EQ(parse_config("fov = 30", "c").config.fov, 30.0f);
    EXPECT_EQ(parse_config("fov = 65", "c").config.fov, 65.0f);
}

TEST(Config, TakesEachFrameRate) {
    for (const unsigned rate : bb::common::frame_rates) {
        EXPECT_EQ(parse_config(std::format("frame_rate = {}", rate), "c").config.frame_rate, rate);
    }
}

TEST(Config, TakesEachAnisotropyLevel) {
    for (const unsigned level : bb::common::anisotropy_levels) {
        EXPECT_EQ(parse_config(std::format("anisotropy = {}", level), "c").config.anisotropy,
                  level);
        Config c;
        c.anisotropy = level;
        EXPECT_EQ(parse_config(bb::common::format_config(c), "c").config, c);
    }
    for (const char* value : {"0", "3", "32", "\"16\"", "game", "16.0"}) {
        try {
            parse_config(std::format("anisotropy = {}", value), "c.toml");
            ADD_FAILURE() << value;
        } catch (const bb::common::ConfigError& e) {
            EXPECT_NE(std::string(e.what()).find("c.toml:1: anisotropy: "), std::string::npos)
                << e.what();
        }
    }
}

TEST(Config, TakesTheResolutionsBbRunTakes) {
    for (const auto& [w, h] : {std::pair{1920u, 1080u}, std::pair{2560u, 1440u},
                               std::pair{3840u, 2160u}, std::pair{2560u, 1080u},
                               std::pair{3440u, 1440u}, std::pair{5120u, 1440u}}) {
        Config c;
        c.resolution = std::pair{w, h};
        EXPECT_EQ(parse_config(std::format("resolution = \"{}x{}\"", w, h), "c").config, c);
        EXPECT_EQ(parse_config(bb::common::format_config(c), "c").config, c);
    }
    for (const char* value : {"\"1280x720\"", "\"1950x1097\"", "\"4480x2520\"",
                              "\"3440x1080\"", "\"2560x1080x1\"", "\"x\"", "2560x1080"}) {
        try {
            parse_config(std::format("resolution = {}", value), "c.toml");
            ADD_FAILURE() << value;
        } catch (const bb::common::ConfigError& e) {
            EXPECT_NE(std::string(e.what()).find("c.toml:1: resolution: "), std::string::npos)
                << e.what();
        }
    }
}

TEST(Config, ExpandsTheHomeDirectoryInGameDir) {
    const char* old = std::getenv("HOME");
    const std::string saved = old ? old : "";
    ASSERT_EQ(::setenv("HOME", "/h", 1), 0);
    EXPECT_EQ(parse_config("game_dir = \"~/bb\"", "c").config.game_dir, fs::path("/h/bb"));
    if (old) {
        ::setenv("HOME", saved.c_str(), 1);
    }
}

TEST(Config, UnknownKeysAreReturnedAndIgnored) {
    const auto parsed = parse_config("colour = \"red\"\nfsr = true\nupdate_dir = 3\n", "c");
    EXPECT_EQ(parsed.config.fsr, true);
    EXPECT_EQ(parsed.unknown_keys, (std::vector<std::string>{"colour", "update_dir"}));
}

TEST(Config, StringEscapes) {
    EXPECT_EQ(parse_config("game_dir = \"/a\\\\b\\\"c\"", "c").config.game_dir, fs::path("/a\\b\"c"));
}

TEST(Config, BadValuesNameFileLineAndKey) {
    EXPECT_EQ(error_of("fsr = true\nfullscreen = yes\n"),
              "c.toml:2: fullscreen: \"yes\" is not true or false");
    EXPECT_NE(error_of("fsr = \"true\"").find("not true or false"), std::string::npos);
    EXPECT_NE(error_of("frame_rate = 45")
                  .find("c.toml:1: frame_rate: \"45\" is not 30, 60, 120 or 144"),
              std::string::npos);
    EXPECT_NE(error_of("frame_rate = \"60\"").find("frame_rate"), std::string::npos);
    EXPECT_NE(error_of("frame_rate = 100").find("frame_rate"), std::string::npos);
    EXPECT_NE(error_of("window_size = \"100x100\"").find("window_size"), std::string::npos);
    EXPECT_NE(error_of("window_size = \"1280\"").find("window_size"), std::string::npos);
    EXPECT_NE(error_of("window_size = 1280x720").find("double quotes"), std::string::npos);
    EXPECT_NE(error_of("rcas_attenuation = 2.5").find("rcas_attenuation"), std::string::npos);
    EXPECT_NE(error_of("rcas_attenuation = nan").find("rcas_attenuation"), std::string::npos);
    EXPECT_NE(error_of("rcas_attenuation = abc").find("rcas_attenuation"), std::string::npos);
    EXPECT_NE(error_of("profile = \"bad name\"").find("profile"), std::string::npos);
    EXPECT_NE(error_of("game_dir = \"relative/dir\"").find("absolute"), std::string::npos);
    EXPECT_NE(error_of("game_dir = /unquoted").find("double quotes"), std::string::npos);
}

TEST(Config, BadSyntax) {
    EXPECT_NE(error_of("fsr").find("c.toml:1: \"fsr\" is not key = value"), std::string::npos);
    EXPECT_NE(error_of("= true").find("no key"), std::string::npos);
    EXPECT_NE(error_of("fsr =").find("no value"), std::string::npos);
    EXPECT_NE(error_of("[window]\n").find("tables are not supported"), std::string::npos);
    EXPECT_NE(error_of("profile = \"abc").find("not closed"), std::string::npos);
    EXPECT_NE(error_of("profile = \"abc\" x").find("unexpected text"), std::string::npos);
    EXPECT_NE(error_of("profile = \"a\\q\"").find("unknown escape"), std::string::npos);
    EXPECT_NE(error_of("fsr = true\nfsr = false\n").find("c.toml:2: fsr is set twice (first on line 1)"),
              std::string::npos);
}

class ConfigFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::string templ = (fs::temp_directory_path() / "bb-config-test-XXXXXX").string();
        ASSERT_NE(::mkdtemp(templ.data()), nullptr);
        dir_ = templ;
    }
    void TearDown() override { fs::remove_all(dir_); }
    fs::path dir_;
};

TEST_F(ConfigFileTest, WriteThenReadGivesTheSameConfig) {
    Config c;
    c.game_dir = fs::path("/games/with \"quotes\"");
    c.profile = "p-1";
    c.window_size = std::pair{1920u, 1080u};
    c.resolution = std::pair{3440u, 1440u};
    c.fullscreen = false;
    c.fsr = true;
    c.rcas = true;
    c.rcas_attenuation = 0.25f;
    c.frame_rate = 30;
    c.motion_blur = false;
    c.motion_blur_strength = 0.3f;
    c.chromatic_aberration = true;
    c.chromatic_aberration_strength = 0.75f;
    c.fov = 62.5f;
    const fs::path file = dir_ / "sub" / "config.toml";  // the directory is created
    bb::common::write_config(file, c);
    EXPECT_EQ(bb::common::read_config(file).config, c);
    EXPECT_FALSE(fs::exists(file.string() + ".tmp"));
    Config partial;
    partial.fsr = true;
    bb::common::write_config(file, partial);  // replaces the file
    EXPECT_EQ(bb::common::read_config(file).config, partial);
}

TEST_F(ConfigFileTest, UpdateKeepsWhatAnotherWriterSet) {
    const fs::path file = dir_ / "config.toml";
    bb::common::update_config(file, [](Config& c) { c.profile = "first"; });  // no file yet
    Config other = bb::common::read_config(file).config;
    other.game_dir = fs::path("/games/bb");
    bb::common::write_config(file, other);  // written in between by someone else
    bb::common::update_config(file, [](Config& c) { c.profile = "second"; });
    const Config got = bb::common::read_config(file).config;
    EXPECT_EQ(got.profile, "second");
    EXPECT_EQ(got.game_dir, fs::path("/games/bb"));
    std::ofstream(file) << "fsr = maybe\n";
    EXPECT_THROW(bb::common::update_config(file, [](Config&) {}), ConfigError);
}

TEST_F(ConfigFileTest, ReadingAMissingFileFails) {
    try {
        bb::common::read_config(dir_ / "none.toml");
        FAIL();
    } catch (const ConfigError& e) {
        EXPECT_NE(std::string(e.what()).find("cannot read the config file"), std::string::npos);
    }
}

TEST_F(ConfigFileTest, WritingBelowAFileFails) {
    std::ofstream(dir_ / "file") << "x";
    EXPECT_THROW(bb::common::write_config(dir_ / "file" / "config.toml", Config{}), ConfigError);
}

TEST_F(ConfigFileTest, DefaultPathIsInTheUserDir) {
    ASSERT_EQ(::setenv("BB_USER_DIR", dir_.c_str(), 1), 0);
    EXPECT_EQ(bb::common::default_config_path(), dir_ / "config.toml");
}

}  // namespace
