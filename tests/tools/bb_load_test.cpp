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

#include "loader/image.hpp"
#include "support/eboot_facts.hpp"
#include "support/game.hpp"
#include "support/process.hpp"

namespace {

using bb::test::quote;
using bb::test::Result;
using bb::test::run;

namespace facts = bb::test::eboot_facts;

using LibraryCounts = std::tuple<std::string, int, int>;  // library, func, object

// The imports of the eboot by library, as bb-load prints them.
std::vector<LibraryCounts> eboot_imports() {
    std::vector<LibraryCounts> out;
    for (const facts::LibraryImports& entry : facts::library_imports) {
        out.emplace_back(entry.library, entry.functions, entry.objects);
    }
    return out;
}

const std::string bb_load = quote(BB_LOAD_PATH);

TEST(BbLoad, NeedsTheGameDirectory) {
    Result r = run("BB_GAME_DIR= " + bb_load + " 2>&1");
    EXPECT_EQ(r.status, 2);
    EXPECT_NE(r.output.find("set BB_GAME_DIR or pass --game-dir"), std::string::npos) << r.output;
    r = run(bb_load + " --game-dir /nonexistent 2>&1");
    EXPECT_EQ(r.status, 1);
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
    ASSERT_EQ(r.status, 0) << r.output;
    std::string last;
    std::vector<ModuleOutput> modules = parse(r.output, last);
    ASSERT_EQ(modules.size(), 3U) << r.output;
    EXPECT_EQ(modules[0].header,
              (std::vector<std::string>{
                  "module     eboot.bin id 1",
                  std::format("base       {:#x}", bb::loader::default_base),
                  std::format("entry      {:#x}", facts::entry),
                  std::format("segment    {:#x} size {:#x} file {:#x} r-x", facts::code.link_addr,
                              facts::code.mem_size, facts::code.file_size),
                  std::format("segment    {:#x} size {:#x} file {:#x} rw-", facts::data.link_addr,
                              facts::data.mem_size, facts::data.file_size),
                  std::format("tls        {:#x} file {:#x} mem {:#x} align {:#x}",
                              facts::tls.link_addr, facts::tls.file_size, facts::tls.mem_size,
                              facts::tls.align),
                  std::format("procparam  {:#x}", facts::proc_param),
                  std::format("reloc      R_X86_64_64 {}", facts::relocs_64),
                  std::format("reloc      R_X86_64_GLOB_DAT {}", facts::relocs_glob_dat),
                  std::format("reloc      R_X86_64_JUMP_SLOT {}", facts::relocs_jump_slot),
                  std::format("reloc      R_X86_64_RELATIVE {}", facts::relocs_relative),
              }));
    EXPECT_EQ(modules[0].imports, std::format("imports    {} in {} libraries", facts::imports,
                                              facts::library_imports.size()));
    EXPECT_EQ(modules[0].libraries, eboot_imports());
    // All libc imports are exported by libc.prx, but some are never referenced.
    const facts::LibraryImports libc = facts::imports_of("libc");
    EXPECT_EQ(modules[0].bindings["libc"],
              std::format("{} 0 0 {}",
                          libc.functions + libc.objects - facts::unreferenced_libc_imports,
                          facts::unreferenced_libc_imports));
    EXPECT_EQ(modules[0].bindings["libSceFios2"],
              std::format("{} 0 0 0", facts::imports_of("libSceFios2").functions));
    // All libkernel imports go to HLE, at least 16 of them to implementations in src/hle/kernel.
    // Bounds rather than exact numbers, so that each new kernel function doesn't edit this line.
    const facts::LibraryImports libkernel = facts::imports_of("libkernel");
    EXPECT_TRUE(hle_at_least(modules[0].bindings["libkernel"],
                             libkernel.functions + libkernel.objects, 16));
    // src/hle/net implements sceNetInit, sceNetPoolCreate and sceNetEpollWait.
    EXPECT_EQ(modules[0].bindings["libSceNet"],
              std::format("0 3 {} 0", facts::imports_of("libSceNet").functions - 3));

    EXPECT_EQ(modules[1].header.at(0), "module     libSceFios2.prx id 2");
    EXPECT_EQ(modules[1].header.at(1), 
              std::format("base       {:#x}", bb::loader::default_base + facts::fios2_offset));
    EXPECT_EQ(modules[1].header.back(),
              std::format("reloc      R_X86_64_DTPMOD64 {}", facts::fios2_dtpmod_relocs));
    EXPECT_EQ(modules[1].imports, "imports    81 in 4 libraries");
    EXPECT_EQ(modules[1].bindings["libSceLibcInternal"], "15 0 1 0");
    EXPECT_EQ(modules[2].header.at(0), "module     libc.prx id 3");
    EXPECT_EQ(modules[2].header.at(1), 
              std::format("base       {:#x}", bb::loader::default_base + facts::libc_offset));
    EXPECT_EQ(modules[2].header.back(),
              std::format("reloc      R_X86_64_DTPMOD64 {}", facts::libc_dtpmod_relocs));
    EXPECT_EQ(modules[2].imports, "imports    79 in 2 libraries");
    EXPECT_EQ(modules[2].bindings["libSceFios2"], "8 0 0 0");
    // At most 468 stubs across the load set; each new HLE function lowers the number.
    std::string word;
    int stubs = -1;
    std::istringstream in(last);
    EXPECT_TRUE(in >> word >> stubs) << last;
    EXPECT_EQ(word, "hle") << last;
    EXPECT_GT(stubs, 0) << last;
    EXPECT_LE(stubs, 468) << last;
    EXPECT_TRUE(last.ends_with(" stubs, 3 data blocks")) << last;
}

}  // namespace
