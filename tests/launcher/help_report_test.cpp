#include <sys/wait.h>

#include <unistd.h>

#include <algorithm>
#include <csignal>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "launcher/help_report.hpp"

namespace {

using namespace bb::launcher;
using bb::common::FolderCheck;

TEST(HelpScrub, HomeBecomesTilde) {
    EXPECT_EQ(scrub("open /ho" "me/alice/Games/bb/eboot.bin failed", "/ho" "me/alice", ""),
              "open ~/Games/bb/eboot.bin failed");
    EXPECT_EQ(scrub("/ho" "me/alice", "/ho" "me/alice/", ""), "~");
    // A longer name that starts with the home path is another directory.
    EXPECT_EQ(scrub("/srv/ho" "me/alicex/f", "/srv/ho" "me/alice", ""), "/srv~/f");
}

TEST(HelpScrub, OtherHomeDirectoriesToo) {
    EXPECT_EQ(scrub("/var/ho" "me/bob/x and /ho" "me/carol.d/y", "/ho" "me/alice", ""), "~/x and ~/y");
    EXPECT_EQ(scrub("/home/ and /home", "", ""), "/home/ and /home");
}

TEST(HelpScrub, UserNameNeverAppears) {
    const std::string out =
        scrub("user alice, /run/media/alice/disk, alice@host, malice, alice_2", "/ho" "me/alice", "alice");
    EXPECT_EQ(out, "user <user>, /run/media/<user>/disk, <user>@host, malice, alice_2");
}

TEST(HelpScrub, RootIsNotReplacedEverywhere) {
    EXPECT_EQ(scrub("root of /root", "/root", "root"), "root of ~");
}

TEST(HelpLines, KeepsTheLastLines) {
    EXPECT_EQ(last_lines("a\nb\nc\n", 2), "b\nc\n");
    EXPECT_EQ(last_lines("a\nb\nc", 2), "b\nc\n");
    EXPECT_EQ(last_lines("a\nb\nc\n", 3), "a\nb\nc\n");
    EXPECT_EQ(last_lines("a\nb\nc\n", 10), "a\nb\nc\n");
    EXPECT_EQ(last_lines("a\n\nc\n", 2), "\nc\n");
    EXPECT_EQ(last_lines("", 5), "");
    EXPECT_EQ(last_lines("x\n", 0), "");
}

TEST(HelpReport, HasTheTwoHundredLastLinesOnlyAndIsScrubbed) {
    ReportInput in;
    in.launcher_version = "v1";
    in.bb_run = "found";
    in.distribution = "Test OS";
    in.kernel = "Linux 7 x86_64";
    in.gpu = "Some GPU on /ho" "me/alice/driver";
    in.last_exit = "exit status 3";
    in.home = "/ho" "me/alice";
    in.user = "alice";
    for (int i = 0; i < 500; ++i) {
        in.log += std::format("line {} /ho" "me/alice/.local/state/x alice\n", i);
    }
    const std::string r = build_report(in);
    EXPECT_EQ(r.find("alice"), std::string::npos);
    EXPECT_EQ(r.find("/home"), std::string::npos);
    EXPECT_NE(r.find("exit status 3"), std::string::npos);
    EXPECT_NE(r.find("line 499 ~/.local/state/x <user>\n"), std::string::npos);
    EXPECT_NE(r.find("line 300 "), std::string::npos);
    EXPECT_EQ(r.find("line 299 "), std::string::npos);
    EXPECT_EQ(std::count(r.begin(), r.end(), '\n'), 9 + 200 - 1 + 1);
}

// The portable folder goes first, so the rest of its path below home doesn't show either.
TEST(HelpReport, TheLaunchersFolderBecomesAPlaceholder) {
    ReportInput in;
    in.home = "/ho" "me/alice";
    in.user = "alice";
    in.folder = "/ho" "me/alice/Games/Bloodborne-Linux/";
    in.log = "open /ho" "me/alice/Games/Bloodborne-Linux/user/logs/x.log\n"
             "other /ho" "me/alice/Games/Bloodborne-Linux2/y\n";
    const std::string r = build_report(in);
    EXPECT_NE(r.find("open <folder>/user/logs/x.log\n"), std::string::npos) << r;
    EXPECT_NE(r.find("other ~/Games/Bloodborne-Linux2/y\n"), std::string::npos) << r;
    EXPECT_EQ(r.find("alice"), std::string::npos) << r;
}

TEST(HelpReport, NoRunNoLog) {
    ReportInput in;
    const std::string r = build_report(in);
    EXPECT_NE(r.find("no run yet"), std::string::npos);
    EXPECT_NE(r.find("(no log)"), std::string::npos);
}

TEST(HelpProblems, TheOneThatAppliesComesFirst) {
    ProblemInput in;
    EXPECT_FALSE(problem_applies(Problem::folder_moved, in));
    EXPECT_EQ(order_problems(in).front(), Problem::folder_moved);  // nothing applies: fixed order
    in.have_folder = true;
    in.folder = FolderCheck::not_a_folder;
    in.last_run_failed = true;
    auto o = order_problems(in);
    EXPECT_EQ(o[0], Problem::folder_moved);
    EXPECT_EQ(o[1], Problem::stopped);
    in.folder = FolderCheck::missing_update;
    in.gpu_ok = false;
    in.last_run_failed = false;
    o = order_problems(in);
    EXPECT_EQ(o[0], Problem::wrong_version);
    EXPECT_EQ(o[1], Problem::no_gpu);
    EXPECT_EQ(o.size(), 4u);
}

TEST(HelpProblems, AGoodFolderIsNoProblem) {
    ProblemInput in;
    in.have_folder = true;
    EXPECT_FALSE(problem_applies(Problem::folder_moved, in));
    EXPECT_FALSE(problem_applies(Problem::wrong_version, in));
    in.gpu_ok = false;
    EXPECT_EQ(order_problems(in).front(), Problem::no_gpu);
}

TEST(HelpCapture, StatusWordsAndTail) {
    EXPECT_EQ(describe_wait_status(3 << 8), "exit status 3");
    EXPECT_EQ(describe_wait_status(SIGSEGV).substr(0, 24), "killed by signal 11 (Seg");
    namespace fs = std::filesystem;
    const fs::path f = fs::temp_directory_path() / std::format("bb-help-tail-{}", getpid());
    std::ofstream(f) << "first\nsecond\nthird\n";
    EXPECT_EQ(read_tail(f, 1000), "first\nsecond\nthird\n");
    EXPECT_EQ(read_tail(f, 9), "third\n");  // cut inside "second": start at the next line
    EXPECT_EQ(read_tail(fs::path(f) += "-none", 10), "");
    fs::remove(f);
}

}  // namespace
