// Runs the bb-load binary and checks its output.

#include <sys/wait.h>

#include <cstdio>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include "support/game.hpp"

namespace {

struct Result {
    int exit_code;  // -1 when the process didn't exit normally
    std::string output;
};

// Runs `command` through /bin/sh and captures its standard output.
Result run(const std::string& command) {
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return {-1, "popen failed"};
    }
    std::string output;
    char buffer[4096];
    while (std::size_t n = std::fread(buffer, 1, sizeof buffer, pipe)) {
        output.append(buffer, n);
    }
    int status = pclose(pipe);
    return {WIFEXITED(status) ? WEXITSTATUS(status) : -1, output};
}

// `s` as a single-quoted shell word.
std::string quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    }
    return out + "'";
}

using LibraryCounts = std::tuple<std::string, int, int>;  // library, func, object

// The imports of eboot.elf by library, in the order of the summary of tools/elfimports.
const std::vector<LibraryCounts> eboot_imports = {
    {"libc", 197, 19},
    {"libkernel", 79, 1},
    {"libSceGnmDriver", 50, 0},
    {"libSceNet", 35, 0},
    {"libScePosix", 32, 0},
    {"libSceNpMatching2", 27, 0},
    {"libSceHttp", 24, 0},
    {"libSceNpManager", 20, 0},
    {"libSceFios2", 17, 0},
    {"libSceNpWebApi", 17, 0},
    {"libSceAvPlayer", 16, 0},
    {"libSceNpScore", 14, 0},
    {"libSceAjm", 12, 0},
    {"libSceSaveData", 11, 0},
    {"libSceVoice", 11, 0},
    {"libSceRtc", 10, 0},
    {"libScePad", 9, 0},
    {"libSceVideoOut", 8, 0},
    {"libSceNpSignaling", 7, 0},
    {"libSceNpUtility", 7, 0},
    {"libSceAudioOut", 6, 0},
    {"libSceNetCtl", 6, 0},
    {"libSceNpTrophy", 6, 0},
    {"libSceUserService", 6, 0},
    {"libSceImeDialog", 5, 0},
    {"libSceNpProfileDialog", 5, 0},
    {"libScePlayGo", 5, 0},
    {"libSceSystemService", 5, 0},
    {"libSceMouse", 4, 0},
    {"libSceMsgDialog", 4, 0},
    {"libSceNpAuth", 4, 0},
    {"libSceNpCommerce", 4, 0},
    {"libSceSaveDataDialog", 4, 0},
    {"libSceAppContent", 3, 0},
    {"libSceAudioIn", 3, 0},
    {"libSceSysmodule", 3, 0},
    {"libSceNpCommon", 2, 0},
    {"libSceSsl", 2, 0},
    {"libSceCommonDialog", 1, 0},
};

const std::string bb_load = quote(BB_LOAD_PATH);

TEST(BbLoad, NeedsTheGameDirectory) {
    Result r = run("BB_GAME_DIR= " + bb_load + " 2>&1");
    EXPECT_EQ(r.exit_code, 2);
    EXPECT_NE(r.output.find("set BB_GAME_DIR or pass --game-dir"), std::string::npos) << r.output;
    r = run(bb_load + " --game-dir /nonexistent 2>&1");
    EXPECT_EQ(r.exit_code, 1);
    EXPECT_NE(r.output.find("/nonexistent/eboot.bin"), std::string::npos) << r.output;
}

TEST(BbLoadGame, PrintsTheEboot) {
    BB_REQUIRE_GAME(game);
    Result r = run(bb_load + " --game-dir " + quote(game.string()));
    ASSERT_EQ(r.exit_code, 0) << r.output;
    std::istringstream lines(r.output);
    std::vector<std::string> header;
    std::string line;
    while (std::getline(lines, line) && !line.starts_with("imports")) {
        header.push_back(line);
    }
    EXPECT_EQ(header, (std::vector<std::string>{
                          "base       0x10000000",
                          "entry      0xa0",
                          "segment    0x0 size 0x50d96dc file 0x50d96dc r-x",
                          "segment    0x50dc000 size 0x5f70f4 file 0x309b38 rw-",
                          "tls        0x50dc040 file 0x0 mem 0x750 align 0x10",
                          "procparam  0x50dc000",
                      }));
    EXPECT_EQ(line, "imports    701 in 39 libraries");

    std::vector<LibraryCounts> imports;
    while (std::getline(lines, line)) {
        std::istringstream in(line);
        std::string library, func_word, object_word;
        int total = 0, func = 0, object = 0;
        in >> library >> total >> func >> func_word >> object >> object_word;
        EXPECT_EQ(total, func + object) << line;
        EXPECT_EQ(func_word + object_word, "func,object") << line;
        imports.emplace_back(library, func, object);
    }
    EXPECT_EQ(imports, eboot_imports);
}

}  // namespace
