// The words for how a run ended, written next to the capture of its output.
#include <sys/wait.h>

#include <csignal>

#include <gtest/gtest.h>

#include "launcher/run_log.hpp"

namespace {

using namespace bb::launcher;

TEST(RunLog, StatusWords) {
    EXPECT_EQ(describe_wait_status(3 << 8), "exit status 3");
    EXPECT_EQ(describe_wait_status(SIGSEGV).substr(0, 24), "killed by signal 11 (Seg");
    EXPECT_EQ(last_run_log("/s").string(), "/s/logs/launcher-last-run.log");
    EXPECT_EQ(last_run_status("/s").string(), "/s/logs/launcher-last-run.status");
}

}  // namespace
