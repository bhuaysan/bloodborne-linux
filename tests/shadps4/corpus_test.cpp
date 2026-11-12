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
#include "support/corpus.hpp"
#include "support/spirv_val.hpp"
#include "shader_recompiler/frontend/copy_shader.h"
#include "shadps4/program_state.hpp"
#include "shadps4_host/failure.hpp"
#include "support/game.hpp"

namespace {

namespace fs = std::filesystem;
using namespace bb::test::gcn;
using namespace bb::test::shadps4;

constexpr std::string_view ok_marker = "RESULT ok";
// What follows ok_marker on its line: the SHA-256 of the SPIR-V module, so two reports show
// whether a translator change altered any module. Some domain shaders translate differently with
// the heap at other addresses, so compare runs made under `setarch -R` (address randomization off).
constexpr std::string_view spirv_marker = " spirv=";
// A copy shader never becomes a module of its own: it is read while translating its geometry shader.
constexpr std::string_view copy_marker = "RESULT copy-parsed";
constexpr unsigned time_limit_seconds = 120;
// What is known not to translate: these programs, by the SHA-256 of their code (the key that
// deduplicates the corpus), each with the cause it fails for. A failure of any other program, a
// listed program that fails for another cause and a listed program that no longer fails all fail
// the test. A fixed program has to leave the list.
struct KnownFailure {
    std::string_view sha256;
    std::string_view cause;
};
constexpr std::string_view hull_cause =
    "hull_shader_transform.cpp:# HullShaderTransform: Assertion Failed!unhandled pattern in tess "
    "factor store";
constexpr std::string_view copy_cause = "copy_shader.cpp:# operator(): Assertion Failed!";
const KnownFailure known_failures[] = {
    {"4a8d09572b70b42167b40b7fb73929772acbe1e16ea65524f2cef43f344ce87b", hull_cause},
    {"55b2005381c36b49bae1a0fb6edf849637bc4e4138ae1770a21150296ca88623", hull_cause},
    {"d9cb21f09c646f2462b109f3bc568c7a87dd052fabd42511adfdeded6055b723", hull_cause},
    {"e221e748e05b1449b983c429f95d7f8f8616455515b692ff7d7eab85ae1a270a", hull_cause},
    {"23030583679d1f3afab511489a789be1d356185abc93d1d8d4a51502a166fa44", copy_cause},
    {"36083e50dfc5a82ba166075dc9f5e8d1c7483ec1f9bd78700583fa2562582f85", copy_cause},
    {"43f205541fb1910e9b5c5f9d04138cb0f214b03c3c7289a22e7eeb6753b9a54b", copy_cause},
    {"fcb07ce8fd8c6981f1fabf6a6b709183edf965f6517191c81f657296f625c382", copy_cause},
    // EmitImageQueryLod indexes ctx.samplers out of bounds. It crashes with and without checked
    // containers (-D_GLIBCXX_ASSERTIONS), so it needs no exception.
    {"a98c9836981b0d3914b711f96fde634c8a5c54d51a382447a7390520f3806dd5", "crash (abort)"},
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
    std::string sha256;
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
            const auto digest = bb::common::sha256(std::as_bytes(std::span(module)));
            finish_job(std::string(ok_marker) + std::string(spirv_marker) + bb::common::to_hex(digest));
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

// The SPIR-V hash a translated program printed, or nothing.
std::string spirv_hash_of(const std::string& output) {
    const auto at = output.find(std::string(ok_marker) + std::string(spirv_marker));
    if (at == std::string::npos) {
        return {};
    }
    const std::size_t from = at + ok_marker.size() + spirv_marker.size();
    return output.substr(from, output.find('\n', from) - from);
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
        Job job{file, std::move(program), {}, bb::common::to_hex(digest)};
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
    BB_REQUIRE_GAME_DATA(root, "tools/shaderbnd");
    if (!have_spirv_val()) {
        GTEST_SKIP() << "spirv-val not found";
    }

    std::vector<Job> jobs = distinct_programs(root);
    // For debugging one program: only the jobs whose file path contains this text run.
    const char* only = getenv("BB_CORPUS_ONLY");
    const bool filtered = only != nullptr;
    if (filtered) {
        std::erase_if(jobs, [&](const Job& job) { return job.file.string().find(only) == std::string::npos; });
    }
    if (!filtered) {
        EXPECT_EQ(jobs.size(), 3832u);
    }

    std::map<std::string, std::uint64_t> total_by_stage, ok_by_stage, by_reason;
    std::map<std::string, const Job*> example;
    std::map<std::string, std::string> cause_by_sha;  // the programs that failed
    // A filtered debugging run must not overwrite the full report.
    std::ofstream report(bb::test::analysis_dir() / (!filtered ? "shadps4-corpus-report.txt"
                                                                       : "shadps4-corpus-report-filtered.txt"));

    const auto finish = [&](const Running& running, const std::string& outcome,
                            const std::string& spirv_hash) {
        const std::string stage = program_type_name(running.job->program.type);
        ++total_by_stage[stage];
        if (outcome.empty()) {
            ++ok_by_stage[stage];
            report << "ok     " << running.job->file.string()
                   << (running.form == TessConstantsForm::through_pointer ? " (second V# form)" : "")
                   << (spirv_hash.empty() ? "" : " spirv " + spirv_hash) << '\n';
            return;
        }
        const std::string reason = reason_of(outcome);
        ++by_reason[reason];
        cause_by_sha[running.job->sha256] = std::string(cause_of(reason));
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
        finish(current, outcome, spirv_hash_of(output));
    }

    std::vector<std::pair<std::uint64_t, std::string>> reasons;
    std::uint64_t failed = 0;
    for (const auto& [reason, count] : by_reason) {
        reasons.emplace_back(count, reason);
        failed += count;
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

    if (!filtered) {
        for (const auto& [sha, cause] : cause_by_sha) {
            const auto known = std::find_if(std::begin(known_failures), std::end(known_failures),
                                            [&](const KnownFailure& k) { return k.sha256 == sha; });
            if (known == std::end(known_failures)) {
                ADD_FAILURE() << "program " << sha << " is not a known failure: " << cause;
            } else {
                EXPECT_EQ(cause, known->cause) << "program " << sha;
            }
        }
        for (const auto& known : known_failures) {
            if (!cause_by_sha.contains(std::string(known.sha256))) {
                ADD_FAILURE() << "program " << known.sha256 << " no longer fails (" << known.cause
                              << "): remove it from known_failures";
            }
        }
    }
}

}  // namespace
