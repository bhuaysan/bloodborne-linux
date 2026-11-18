// The Help page's report as the page builds it from this machine, against the repository's own
// privacy patterns (tools/githooks/privacy-lib.sh, with .git/info/privacy-patterns).
#include <pwd.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "common/runtime_dirs.hpp"
#include "launcher/help_report.hpp"
#include "launcher/page.hpp"
#include "support/process.hpp"

namespace {

namespace fs = std::filesystem;
using namespace bb::launcher;

TEST(HelpPage, TheReportOfThisMachineHoldsNoPersonalData) {
    const passwd* pw = getpwuid(getuid());
    ASSERT_NE(pw, nullptr);
    const fs::path dir = fs::temp_directory_path() / std::format("bb-help-state-{}", getpid());
    fs::create_directories(dir / "logs");
    {
        std::ofstream log(last_run_log(dir));
        for (int i = 0; i < 300; ++i) {
            log << std::format("[loader] opening {}/Games/bb/eboot.bin as {} (line {})\n", pw->pw_dir,
                               pw->pw_name, i);
        }
        std::ofstream(last_run_status(dir)) << "exit status 1\n";
    }
    Context ctx;
    ctx.state_dir = dir;
    // Not the real probe: the Vulkan driver leaks allocations of its own under LeakSanitizer.
    ctx.gpu_description = std::format("Test GPU (driver in {}/lib)", pw->pw_dir);
    const std::string report = collect_problem_report(ctx);
    fs::remove_all(dir);

    EXPECT_NE(report.find("exit status 1"), std::string::npos) << report;
    EXPECT_NE(report.find("line 299)"), std::string::npos);
    EXPECT_EQ(report.find("line 99)"), std::string::npos);
    EXPECT_EQ(report.find(pw->pw_dir), std::string::npos) << report;
    if (std::string(pw->pw_name) != "root") {
        EXPECT_EQ(report.find(pw->pw_name), std::string::npos) << report;
    }
    if (const char* out = std::getenv("BB_HELP_REPORT_OUT")) {  // for the issue's evidence
        std::ofstream(out) << report;
    }

    // The patterns of the privacy check; skipped when the checkout or git is not there.
    const fs::path file = fs::temp_directory_path() / std::format("bb-help-report-{}.txt", getpid());
    std::ofstream(file) << report;
    const auto r = bb::test::run(std::format(
        "cd {} && . tools/githooks/privacy-lib.sh 2>/dev/null && pat=\"$(privacy_pattern)\" && "
        "[ -n \"$pat\" ] && if grep -Eq -e \"$pat\" {}; then echo MATCH; else echo CLEAN; fi",
        bb::test::quote(bb::common::source_dir().string()), bb::test::quote(file.string())));
    fs::remove(file);
    if (r.output.find("CLEAN") == std::string::npos && r.output.find("MATCH") == std::string::npos) {
        GTEST_SKIP() << "privacy patterns not available";
    }
    EXPECT_EQ(r.output, "CLEAN\n") << "the report matches a privacy pattern";
}

}  // namespace
