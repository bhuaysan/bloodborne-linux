#include "common/file.hpp"

#include <fstream>
#include <string>
#include <system_error>

#include <unistd.h>

#include <gtest/gtest.h>

namespace {

namespace fs = std::filesystem;

class ReadFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::string templ = (fs::temp_directory_path() / "bb-file-test-XXXXXX").string();
        ASSERT_NE(::mkdtemp(templ.data()), nullptr);
        dir_ = templ;
    }
    // Runs also when the test body fails or throws.
    void TearDown() override {
        if (!dir_.empty()) {
            fs::remove_all(dir_);
        }
    }

    fs::path dir_;
};

TEST_F(ReadFileTest, ReadsWholeFile) {
    std::string text(100'000, 'x');
    text += "end";
    std::ofstream out(dir_ / "data", std::ios::binary);
    out << text;
    out.close();
    ASSERT_TRUE(out) << "cannot write " << (dir_ / "data");

    auto data = bb::common::read_file(dir_ / "data");
    ASSERT_EQ(data.size(), text.size());
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(data.data()), data.size()), text);
}

TEST(ReadFile, NamesTheMissingFile) {
    try {
        bb::common::read_file("/nonexistent/bb-file-test");
        FAIL() << "no exception";
    } catch (const std::system_error& e) {
        EXPECT_NE(std::string(e.what()).find("/nonexistent/bb-file-test"), std::string::npos);
    }
}

}  // namespace
