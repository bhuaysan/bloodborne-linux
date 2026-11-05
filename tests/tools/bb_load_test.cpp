// Runs the bb-load binary and checks its output.

#include <sys/wait.h>

#include <cstdio>
#include <format>
#include <map>
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

// One module of the output: the lines up to "imports", that line, and the imports by library.
struct ModuleOutput {
    std::vector<std::string> header;
    std::string imports;
    std::vector<LibraryCounts> libraries;
    std::map<std::string, std::string> bindings;  // library: "<export> <hle> <stub> <unused>"
};

// Splits the output into modules, and returns the last line (the hle counts) in `last`.
std::vector<ModuleOutput> parse(const std::string& output, std::string& last) {
    std::vector<ModuleOutput> modules;
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        last = line;
        if (line.starts_with("module")) {
            modules.emplace_back();
        }
        if (modules.empty() || line.starts_with("hle")) {
            continue;
        }
        ModuleOutput& m = modules.back();
        if (line.starts_with("imports")) {
            m.imports = line;
        } else if (m.imports.empty()) {
            m.header.push_back(line);
        } else {
            std::istringstream in(line);
            std::string library, words[6];
            int total = 0, func = 0, object = 0, exported = 0, hle = 0, stub = 0, unused = 0;
            in >> library >> total >> func >> words[0] >> object >> words[1] >> exported >>
                words[2] >> hle >> words[3] >> stub >> words[4] >> unused >> words[5];
            EXPECT_EQ(total, func + object) << line;
            EXPECT_EQ(total, exported + hle + stub + unused) << line;
            EXPECT_EQ(words[0] + words[1] + words[2] + words[3] + words[4] + words[5],
                      "func,object;export,hle,stub,unused")
                << line;
            m.libraries.emplace_back(library, func, object);
            m.bindings[library] = std::format("{} {} {} {}", exported, hle, stub, unused);
        }
    }
    return modules;
}

// Checks a "<export> <hle> <stub> <unused>" binding of a library whose imports all go to HLE:
// none exported or unused, `total` in all, at least `implemented` of them implemented.
testing::AssertionResult hle_at_least(const std::string& binding, int total, int implemented) {
    int exported = -1, hle = -1, stub = -1, unused = -1;
    std::istringstream(binding) >> exported >> hle >> stub >> unused;
    if (exported == 0 && unused == 0 && hle + stub == total && hle >= implemented) {
        return testing::AssertionSuccess();
    }
    return testing::AssertionFailure() << "binding \"" << binding << "\"";
}

TEST(BbLoadGame, PrintsTheLoadSet) {
    BB_REQUIRE_GAME(game);
    Result r = run(bb_load + " --game-dir " + quote(game.string()));
    ASSERT_EQ(r.exit_code, 0) << r.output;
    std::string last;
    std::vector<ModuleOutput> modules = parse(r.output, last);
    ASSERT_EQ(modules.size(), 3U) << r.output;
    // The relocation counts are those of the tables that tools/common/ps4elf.py reads; 686 of
    // the 701 imports are referenced, 20 of them data.
    EXPECT_EQ(modules[0].header, (std::vector<std::string>{
                                     "module     eboot.bin id 1",
                                     "base       0x10000000",
                                     "entry      0xa0",
                                     "segment    0x0 size 0x50d96dc file 0x50d96dc r-x",
                                     "segment    0x50dc000 size 0x5f70f4 file 0x309b38 rw-",
                                     "tls        0x50dc040 file 0x0 mem 0x750 align 0x10",
                                     "procparam  0x50dc000",
                                     "reloc      R_X86_64_64 2838",
                                     "reloc      R_X86_64_GLOB_DAT 23",
                                     "reloc      R_X86_64_JUMP_SLOT 661",
                                     "reloc      R_X86_64_RELATIVE 231478",
                                 }));
    EXPECT_EQ(modules[0].imports, "imports    701 in 39 libraries");
    EXPECT_EQ(modules[0].libraries, eboot_imports);
    // 14 of the libc imports are never referenced.
    EXPECT_EQ(modules[0].bindings["libc"], "202 0 0 14");
    EXPECT_EQ(modules[0].bindings["libSceFios2"], "17 0 0 0");
    // All 80 libkernel imports go to HLE, at least 16 of them to implementations in
    // src/hle/kernel. Bounds rather than exact numbers, so that each new kernel function
    // doesn't edit this line.
    EXPECT_TRUE(hle_at_least(modules[0].bindings["libkernel"], 80, 16));
    // src/hle/net implements sceNetInit and sceNetPoolCreate.
    EXPECT_EQ(modules[0].bindings["libSceNet"], "0 2 33 0");

    EXPECT_EQ(modules[1].header.at(0), "module     libSceFios2.prx id 2");
    EXPECT_EQ(modules[1].header.at(1), "base       0x156d4000");
    EXPECT_EQ(modules[1].header.back(), "reloc      R_X86_64_DTPMOD64 3");
    EXPECT_EQ(modules[1].imports, "imports    81 in 4 libraries");
    EXPECT_EQ(modules[1].bindings["libSceLibcInternal"], "15 0 1 0");
    EXPECT_EQ(modules[2].header.at(0), "module     libc.prx id 3");
    EXPECT_EQ(modules[2].header.at(1), "base       0x1584c000");
    EXPECT_EQ(modules[2].header.back(), "reloc      R_X86_64_DTPMOD64 14");
    EXPECT_EQ(modules[2].imports, "imports    79 in 2 libraries");
    EXPECT_EQ(modules[2].bindings["libSceFios2"], "8 0 0 0");
    // At most 468 stubs across the load set; each new HLE function lowers the number.
    std::string word;
    int stubs = -1;
    std::istringstream(last) >> word >> stubs;
    EXPECT_EQ(word, "hle") << last;
    EXPECT_LE(stubs, 468) << last;
    EXPECT_TRUE(last.ends_with(" stubs, 3 data blocks")) << last;
}

}  // namespace
