// Translates every distinct program of the shader corpus with the translator derived from
// shadPS4 and validates the SPIR-V (issue #417, docs/design/shader-corpus.md). Each program runs
// in a process of its own, so an assertion or a crash ends that program only. The report goes to
// analysis/shadps4-corpus-report.txt and is never committed.

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "common/sha256.hpp"
#include "gpu/gcn/corpus.hpp"
#include "gpu/gcn/spirv_val.hpp"
#include "shader_recompiler/frontend/copy_shader.h"
#include "shadps4/program_state.hpp"
#include "shadps4_host/failure.hpp"
#include "support/game.hpp"

namespace {

namespace fs = std::filesystem;
using namespace bb::test::gcn;
using namespace bb::test::shadps4;

constexpr std::string_view ok_marker = "RESULT ok";
// A copy shader never becomes a module of its own: it is read while translating its geometry shader.
constexpr std::string_view copy_marker = "RESULT copy-parsed";
constexpr unsigned time_limit_seconds = 120;
// What is known not to translate, by cause, with how many programs may fail for it (fewer is
// fine, docs/design/shader-corpus.md). Anything else fails the test: an invalid module, another
// assertion or another kind of crash. The abort is an out-of-bounds lookup in the translator,
// which only the build with checked containers reports as such.
const std::map<std::string_view, std::uint64_t> known_failures = {
    {"hull_shader_transform.cpp:# HullShaderTransform: Assertion Failed!unhandled pattern in tess "
     "factor store",
     4},
    {"copy_shader.cpp:# operator(): Assertion Failed!", 4},
    {"crash (abort)", 1},
};

[[noreturn]] void finish_job(std::string_view marker) {
    const std::string line = std::string(marker) + '\n';
    (void)!write(STDOUT_FILENO, line.data(), line.size());
    _exit(0);
}

struct Job {
    fs::path file;
    Program program;
    std::vector<std::uint32_t> copy_shader;
};

struct Running {
    pid_t pid;
    int output;
    const Job* job;
    TessConstantsForm form;
};

// Runs in the child. What it prints reaches the parent, which also sees what a crash leaves.
[[noreturn]] void run_job(const Job& job, TessConstantsForm form) {
    alarm(time_limit_seconds);
    install_walker_fault_handler();
    try {
        if (job.program.type == static_cast<unsigned>(ProgramType::copy_vs)) {
            (void)Shader::ParseCopyShader(job.program.code);
            finish_job(copy_marker);
        }
        const auto module = translate(job.program.code, runtime_info_of(job.program, job.copy_shader),
                                      user_data_of(job.program, form));
        const std::string invalid = validate(module);
        if (invalid.empty()) {
            finish_job(ok_marker);
        }
        report_failure("spirv-val: " + invalid);
    } catch (const bb::shader::TranslationFailure& failure) {
        report_failure(failure.what());
    } catch (const std::exception& error) {
        report_failure(std::string("exception: ") + error.what());
    }
}

Running start_job(const Job& job, TessConstantsForm form) {
    int fds[2];
    if (pipe(fds) != 0) {
        throw std::runtime_error("pipe failed");
    }
    const pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error("fork failed");
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        run_job(job, form);
    }
    close(fds[1]);
    return {pid, fds[0], &job, form};
}

std::string read_all(int fd) {
    std::string text;
    char buffer[4096];
    ssize_t count = 0;
    while ((count = read(fd, buffer, sizeof buffer)) > 0) {
        text.append(buffer, static_cast<std::size_t>(count));
    }
    close(fd);
    return text;
}

// What a crash left behind: the most telling line of the child's output.
std::string crash_reason(const std::string& output, int status) {
    std::string best;
    std::size_t start = 0;
    while (start < output.size()) {
        std::size_t end = output.find('\n', start);
        if (end == std::string::npos) {
            end = output.size();
        }
        const std::string line = output.substr(start, end - start);
        if (line.find("AddressSanitizer") != std::string::npos ||
            line.find("runtime error") != std::string::npos ||
            line.find("Assertion") != std::string::npos ||
            line.find("terminate called") != std::string::npos) {
            best = line;
            break;
        }
        start = end + 1;
    }
    const std::string how = !WIFSIGNALED(status)     ? "exit " + std::to_string(WEXITSTATUS(status))
                            : WTERMSIG(status) == SIGABRT ? "abort"
                            : WTERMSIG(status) == SIGSEGV ? "segfault"
                                                          : "signal " + std::to_string(WTERMSIG(status));
    return "crash (" + how + ")" + (best.empty() ? "" : ": " + best);
}

std::string outcome_of(const std::string& output, int status) {
    if (output.find(ok_marker) != std::string::npos ||
        output.find(copy_marker) != std::string::npos) {
        return {};
    }
    const auto at = output.find(failed_marker);
    if (at != std::string::npos) {
        const std::size_t from = at + failed_marker.size();
        return output.substr(from, output.find('\n', from) - from);
    }
    return crash_reason(output, status);
}

// Groups messages by cause: numbers, addresses and paths up to the last '/' go.
std::string reason_of(std::string message) {
    std::string out;
    for (std::size_t i = 0; i < message.size();) {
        const char c = message[i];
        if (c == '0' && i + 1 < message.size() && message[i + 1] == 'x') {
            i += 2;
            while (i < message.size() && std::isxdigit(static_cast<unsigned char>(message[i]))) {
                ++i;
            }
            out += '#';
        } else if (std::isdigit(static_cast<unsigned char>(c)) && (out.empty() || !std::isalpha(static_cast<unsigned char>(out.back())))) {
            while (i < message.size() && std::isdigit(static_cast<unsigned char>(message[i]))) {
                ++i;
            }
            out += '#';
        } else {
            out += c;
            ++i;
        }
    }
    const auto slash = out.rfind('/', out.find(' '));
    return slash == std::string::npos ? out : out.substr(slash + 1);
}

// The key of a reason in known_failures: a crash is known by its kind, whatever it printed.
std::string_view cause_of(std::string_view reason) {
    return reason.starts_with("crash") ? reason.substr(0, reason.find(')') + 1) : reason;
}

std::vector<Job> distinct_programs(const fs::path& root) {
    std::vector<Job> jobs;
    std::map<bb::common::Sha256Digest, std::size_t> by_code;
    for (const auto& file : program_files(root)) {
        std::string why;
        Program program = read_program(file, why);
        if (!why.empty()) {
            ADD_FAILURE() << file.string() << ": " << why;
            continue;
        }
        const auto digest = bb::common::sha256(std::as_bytes(std::span(program.code)));
        Job job{file, std::move(program), {}};
        if (job.program.type == static_cast<unsigned>(ProgramType::gs)) {
            fs::path copy = file;
            copy.replace_extension(".1");
            std::string copy_why;
            if (fs::exists(copy)) {
                job.copy_shader = read_program(copy, copy_why).code;
            }
        }
        const auto [it, fresh] = by_code.try_emplace(digest, jobs.size());
        if (fresh) {
            jobs.push_back(std::move(job));
        } else if (!jobs[it->second].program.has_block && job.program.has_block) {
            jobs[it->second] = std::move(job);  // the same code, with its registers
        }
    }
    return jobs;
}

TEST(ShaderCorpus, EveryProgramTranslatesOrHasAReason) {
    BB_REQUIRE_GAME(game);
    (void)game;
    const auto root = bb::test::analysis_dir() / "shaders";
    if (!fs::exists(root)) {
        GTEST_SKIP() << "analysis/shaders was not extracted";
    }
    if (!have_spirv_val()) {
        GTEST_SKIP() << "spirv-val not found";
    }

    std::vector<Job> jobs = distinct_programs(root);
    // For debugging one program: only the jobs whose file path contains this text run.
    if (const char* only = getenv("BB_CORPUS_ONLY")) {
        std::erase_if(jobs, [&](const Job& job) { return job.file.string().find(only) == std::string::npos; });
    }
    if (getenv("BB_CORPUS_ONLY") == nullptr) {
        EXPECT_EQ(jobs.size(), 3832u);
    }

    std::map<std::string, std::uint64_t> total_by_stage, ok_by_stage, by_reason;
    std::map<std::string, const Job*> example;
    // A filtered debugging run must not overwrite the full report.
    std::ofstream report(bb::test::analysis_dir() / (getenv("BB_CORPUS_ONLY") == nullptr
                                                         ? "shadps4-corpus-report.txt"
                                                         : "shadps4-corpus-report-filtered.txt"));

    const auto finish = [&](const Running& running, const std::string& outcome) {
        const std::string stage = program_type_name(running.job->program.type);
        ++total_by_stage[stage];
        if (outcome.empty()) {
            ++ok_by_stage[stage];
            report << "ok     " << running.job->file.string()
                   << (running.form == TessConstantsForm::through_pointer ? " (second V# form)" : "")
                   << '\n';
            return;
        }
        const std::string reason = reason_of(outcome);
        ++by_reason[reason];
        example.try_emplace(reason, running.job);
        report << "failed " << running.job->file.string() << ": " << outcome << '\n';
    };

    const std::size_t workers = std::max(1u, std::thread::hardware_concurrency() / 2);
    std::vector<Running> running;
    std::size_t next = 0;
    while (next < jobs.size() || !running.empty()) {
        while (next < jobs.size() && running.size() < workers) {
            running.push_back(start_job(jobs[next++], TessConstantsForm::inline_v_sharp));
        }
        Running current = running.front();
        running.erase(running.begin());
        const std::string output = read_all(current.output);
        int status = 0;
        waitpid(current.pid, &status, 0);
        const std::string outcome = outcome_of(output, status);
        const auto type = static_cast<ProgramType>(current.job->program.type);
        if (outcome.starts_with("crash") && current.form == TessConstantsForm::inline_v_sharp &&
            (type == ProgramType::hs || type == ProgramType::ds)) {
            running.push_back(start_job(*current.job, TessConstantsForm::through_pointer));
            continue;
        }
        finish(current, outcome);
    }

    std::vector<std::pair<std::uint64_t, std::string>> reasons;
    std::uint64_t failed = 0;
    std::map<std::string, std::uint64_t> by_cause;
    for (const auto& [reason, count] : by_reason) {
        reasons.emplace_back(count, reason);
        failed += count;
        by_cause[std::string(cause_of(reason))] += count;
    }
    std::sort(reasons.rbegin(), reasons.rend());

    const std::uint64_t parsed = ok_by_stage["copy-vs"];
    std::ostringstream summary;
    summary << "programs: " << jobs.size() << ", valid modules: " << jobs.size() - failed - parsed
            << ", copy shaders parsed: " << parsed << ", not translated: " << failed << '\n';
    for (const auto& [stage, total] : total_by_stage) {
        summary << "  " << stage << ": " << ok_by_stage[stage] << " of " << total << '\n';
    }
    for (const auto& [count, reason] : reasons) {
        summary << "  " << count << "  " << reason << '\n';
    }
    std::cout << summary.str();
    report << '\n' << summary.str();

    if (getenv("BB_CORPUS_ONLY") == nullptr) {
        for (const auto& [cause, count] : by_cause) {
            const auto known = known_failures.find(cause);
            EXPECT_NE(known, known_failures.end()) << count << " programs: " << cause;
            if (known != known_failures.end()) {
                EXPECT_LE(count, known->second) << cause;
            }
        }
    }
}

}  // namespace
