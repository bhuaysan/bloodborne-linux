#include "common/file.hpp"

#include <fstream>
#include <string>
#include <system_error>

#include <unistd.h>

#include <gtest/gtest.h>

namespace {

namespace fs = std::filesystem;

TEST(ReadFile, ReadsWholeFile) {
    std::string templ = (fs::temp_directory_path() / "bb-file-test-XXXXXX").string();
    ASSERT_NE(::mkdtemp(templ.data()), nullptr);
    fs::path dir = templ;
    std::string text(100'000, 'x');
    text += "end";
    std::ofstream(dir / "data") << text;

    auto data = bb::common::read_file(dir / "data");
    fs::remove_all(dir);
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
