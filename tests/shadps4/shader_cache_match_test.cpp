// Measures how many shader modules a game run translated could have been translated ahead of
// time from the shader corpus (issue #512, docs/design/shader-corpus.md). For each module in the
// cache of a game run (BB_SHADER_CACHE_DIR: the directory with its .meta and .spv files), the
// program with the same shader hash is translated from the corpus with the corpus's state and the
// run's device profile. A module could only come from the corpus when its specialization, the key
// the renderer looks a module up by, equals the run's and its SPIR-V is the same bytes. Skips
// without the variable, the game or the extracted corpus. Each module runs in a process of its
// own, as in the corpus test.

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "common/file.hpp"
#include "common/serdes.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/specialization.h"
#include "shadps4/program_state.hpp"
#include "shadps4_host/failure.hpp"
#include "support/corpus.hpp"
#include "support/game.hpp"

namespace {

namespace fs = std::filesystem;
using namespace bb::test::gcn;
using namespace bb::test::shadps4;

constexpr std::size_t blob_header_size = 24;  // Storage's BlobHeader
constexpr std::string_view result_marker = "RESULT ";

std::vector<std::uint8_t> read_blob(const fs::path& file) {
    auto bytes = bb::common::read_file(file);
    if (bytes.size() < blob_header_size) {
        return {};
    }
    std::vector<std::uint8_t> payload(bytes.size() - blob_header_size);
    std::memcpy(payload.data(), bytes.data() + blob_header_size, payload.size());
    return payload;
}

// The shader hash in a program file's footer block (BinaryInfo::shader_hash, at +16).
std::uint64_t shader_hash_of(const fs::path& file) {
    const auto bytes = bb::common::read_file(file);
    const std::string_view data(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const auto block = data.find("OrbShdr");
    std::uint64_t hash = 0;
    if (block != std::string_view::npos && block + 24 <= data.size()) {
        std::memcpy(&hash, data.data() + block + 16, sizeof(hash));
    }
    return hash;
}

// Every part of the run's specialization the corpus's differs in, joined by '+', or "" when the
// two are equal as the renderer compares them.
std::string spec_difference(const Shader::StageSpecialization& corpus,
                            const Shader::StageSpecialization& game) {
    if (corpus == game) {
        return {};
    }
    std::vector<std::string> parts;
    const auto& a = corpus.runtime_info;
    const auto& b = game.runtime_info;
    if (a.hw_stage != b.hw_stage || a.sw_stage != b.sw_stage) {
        parts.emplace_back("stage");
    } else {
        if (!(a.props == b.props)) {
            parts.emplace_back("registers");
        }
        if (!a.HasSameSwInfo(b.sw)) {
            parts.emplace_back("sw-draw-state");
        }
        if (a.hw_stage == Shader::HwStage::Fragment) {
            if (!std::ranges::equal(a.hw.fs.color_buffers, b.hw.fs.color_buffers)) {
                parts.emplace_back("render-targets");
            }
            auto x = a.hw.fs;
            x.color_buffers = b.hw.fs.color_buffers;
            if (!(x == b.hw.fs)) {
                parts.emplace_back("pixel-state");
            }
        } else if (!a.HasSameHwInfo(b.hw)) {
            parts.emplace_back("hw-draw-state");
        }
    }
    if (corpus.fetch_shader_data != game.fetch_shader_data || corpus.vs_attribs != game.vs_attribs) {
        parts.emplace_back("fetch-shader");
    }
    if (corpus.fmasks != game.fmasks) {
        parts.emplace_back("fmasks");
    }
    const bool resources = !corpus.buffers.empty() || !corpus.images.empty() ||
                           !corpus.samplers.empty();
    if (resources && corpus.start != game.start) {
        parts.emplace_back("binding-start");
    }
    if (corpus.buffers.size() != game.buffers.size() || corpus.images.size() != game.images.size() ||
        corpus.samplers.size() != game.samplers.size()) {
        parts.emplace_back("resource-count");
    } else {
        for (std::size_t i = 0; i < corpus.buffers.size(); ++i) {
            if (game.bitset[i] && corpus.buffers[i] != game.buffers[i]) {
                parts.emplace_back("buffer-sharps");
                break;
            }
        }
        for (std::size_t i = 0; i < corpus.images.size(); ++i) {
            if (game.bitset[corpus.buffers.size() + i] && corpus.images[i] != game.images[i]) {
                parts.emplace_back("image-sharps");
                break;
            }
        }
        if (corpus.samplers != game.samplers) {
            parts.emplace_back("sampler-sharps");
        }
    }
    std::string joined;
    for (const auto& part : parts) {
        joined += (joined.empty() ? "" : "+") + part;
    }
    return joined.empty() ? "other" : joined;
}

// A module's meta file, as LoadShaderMeta reads it: the versions, the permutation, then the
// specialization and the program's info.
void read_meta(const fs::path& file, Shader::Info& info, Shader::StageSpecialization& spec,
               std::size_t& perm_idx) {
    Serialization::Archive ar{read_blob(file)};
    Serialization::Reader meta{ar};
    std::uint32_t meta_version = 0, binary_version = 0;
    std::uint64_t perm_hash = 0;
    meta.Read(meta_version);
    meta.Read(binary_version);
    meta.Read(perm_hash);
    meta.Read(perm_idx);
    spec.Deserialize(ar);
    info.Deserialize(ar);
}

// The shader hashes of every footer block in `file`: the programs the executable holds itself.
std::set<std::uint64_t> embedded_hashes(const fs::path& file) {
    std::set<std::uint64_t> hashes;
    if (!fs::is_regular_file(file)) {
        return hashes;
    }
    const auto bytes = bb::common::read_file(file);
    const std::string_view data(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    for (auto at = data.find("OrbShdr"); at != std::string_view::npos && at + 24 <= data.size();
         at = data.find("OrbShdr", at + 1)) {
        std::uint64_t hash = 0;
        std::memcpy(&hash, data.data() + at + 16, sizeof(hash));
        hashes.insert(hash);
    }
    return hashes;
}

struct Corpus {
    fs::path file;
    Program program;
    std::vector<std::uint32_t> copy_shader;
};

[[noreturn]] void finish(const std::string& line) {
    const std::string out = std::string(result_marker) + line + '\n';
    (void)!write(STDOUT_FILENO, out.data(), out.size());
    _exit(0);
}

// In the child: compares one module of the run with the corpus's translation of its program.
[[noreturn]] void compare(const fs::path& meta_file, const fs::path& cache, const Corpus& corpus,
                          const Shader::Profile& profile, TessConstantsForm form) {
    alarm(120);
    install_walker_fault_handler();
    Shader::Info game_info{};
    Shader::StageSpecialization game_spec{};
    game_spec.info = &game_info;
    std::size_t perm_idx = 0;
    read_meta(meta_file, game_info, game_spec, perm_idx);
    const auto spv_bytes =
        read_blob(cache / std::format("{:#018x}_{}.spv", game_info.pgm_hash, perm_idx));
    std::vector<std::uint32_t> game_spv(spv_bytes.size() / 4);
    std::memcpy(game_spv.data(), spv_bytes.data(), game_spv.size() * 4);

    const std::string stage = std::format("{}/{}", static_cast<int>(game_info.hw_stage),
                                          static_cast<int>(game_info.sw_stage));
    try {
        Shader::RuntimeInfo runtime_info = runtime_info_of(corpus.program, corpus.copy_shader);
        const UserData user_data = user_data_of(corpus.program, form);
        const Shader::ShaderParams params{
            .user_data = user_data, .code = corpus.program.code, .hash = game_info.pgm_hash};
        Shader::Info info(runtime_info.hw_stage, runtime_info.sw_stage, params);
        Shader::Pools pools;
        const auto ir = Shader::TranslateProgram(corpus.program.code, pools, info, runtime_info,
                                                 profile);
        Shader::Backend::Bindings bindings{};
        const auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, ir, bindings);
        const Shader::StageSpecialization spec(info, runtime_info, profile, {});
        const std::string diff = spec_difference(spec, game_spec);
        finish(std::format("{} perm{} {} {}", stage, perm_idx, diff.empty() ? "key-match" : diff,
                           spv == game_spv ? "spirv-same" : "spirv-differs"));
    } catch (const bb::shader::TranslationFailure& failure) {
        finish(stage + " perm" + std::to_string(perm_idx) + " translation-failed -");
    }
}

TEST(ShaderCacheMatch, RunModulesAgainstTheCorpus) {
    const char* cache_env = std::getenv("BB_SHADER_CACHE_DIR");
    if (cache_env == nullptr) {
        GTEST_SKIP() << "BB_SHADER_CACHE_DIR not set";
    }
    BB_REQUIRE_GAME(game);
    (void)game;
    const auto root = bb::test::analysis_dir() / "shaders";
    BB_REQUIRE_GAME_DATA(root, "tools/shaderbnd");
    const fs::path cache = cache_env;

    const auto profile_bytes = read_blob(cache / "profile.bin");
    ASSERT_EQ(profile_bytes.size(), sizeof(Shader::Profile));
    Shader::Profile profile{};
    std::memcpy(&profile, profile_bytes.data(), sizeof(profile));

    std::map<std::uint64_t, Corpus> by_hash;
    for (const auto& file : program_files(root)) {
        std::string why;
        Program program = read_program(file, why);
        if (!why.empty()) {
            continue;
        }
        Corpus entry{file, std::move(program), {}};
        if (entry.program.type == static_cast<unsigned>(ProgramType::gs)) {
            fs::path copy = file;
            copy.replace_extension(".1");
            if (fs::exists(copy)) {
                entry.copy_shader = read_program(copy, why).code;
            }
        }
        auto [it, fresh] = by_hash.try_emplace(shader_hash_of(file), std::move(entry));
        if (!fresh && !it->second.program.has_block && entry.program.has_block) {
            it->second = std::move(entry);
        }
    }

    const auto embedded = embedded_hashes(bb::test::analysis_dir() / "elf" / "eboot.elf");

    std::map<std::string, unsigned> counts;
    unsigned modules = 0;
    unsigned key_matches = 0;
    for (const auto& entry : fs::directory_iterator(cache)) {
        if (entry.path().extension() != ".meta") {
            continue;
        }
        ++modules;
        // The meta's name is its permutation hash; the program hash is inside it.
        Shader::Info probe{};
        Shader::StageSpecialization probe_spec{};
        std::size_t probe_perm = 0;
        read_meta(entry.path(), probe, probe_spec, probe_perm);
        const auto corpus = by_hash.find(probe.pgm_hash);
        if (corpus == by_hash.end()) {
            const char* where = embedded.contains(probe.pgm_hash) ? "in-eboot" : "unknown-origin";
            ++counts[std::format("not-in-archives {}", where)];
            ++counts[std::format("not-in-archives stage {}/{}", static_cast<int>(probe.hw_stage),
                                 static_cast<int>(probe.sw_stage))];
            std::cout << entry.path().filename().string()
                      << std::format(" program {:#018x} {}", probe.pgm_hash, where) << '\n';
            continue;
        }
        std::string line;
        for (const auto form : {TessConstantsForm::inline_v_sharp, TessConstantsForm::through_pointer}) {
            int fds[2];
            ASSERT_EQ(pipe(fds), 0);
            const pid_t pid = fork();
            ASSERT_GE(pid, 0);
            if (pid == 0) {
                close(fds[0]);
                dup2(fds[1], STDOUT_FILENO);
                dup2(fds[1], STDERR_FILENO);
                compare(entry.path(), cache, corpus->second, profile, form);
            }
            close(fds[1]);
            std::string output;
            char buffer[4096];
            ssize_t n = 0;
            while ((n = read(fds[0], buffer, sizeof buffer)) > 0) {
                output.append(buffer, static_cast<std::size_t>(n));
            }
            close(fds[0]);
            int status = 0;
            waitpid(pid, &status, 0);
            const auto at = output.find(result_marker);
            if (at != std::string::npos) {
                line = output.substr(at + result_marker.size());
                line = line.substr(0, line.find('\n'));
                break;
            }
            line = "crash";
        }
        std::cout << entry.path().filename().string() << " " << corpus->second.file.filename().string()
                  << " " << line << '\n';
        // stage perm reason spirv
        std::istringstream words(line);
        std::string stage, perm, reason, spirv;
        words >> stage >> perm >> reason >> spirv;
        key_matches += reason == "key-match" ? 1 : 0;
        ++counts[reason + " " + spirv];
        ++counts["stage " + stage + " " + reason];
    }
    std::cout << "modules: " << modules << ", whose key a corpus translation matches: "
              << key_matches << '\n';
    for (const auto& [what, count] : counts) {
        std::cout << "  " << count << "  " << what << '\n';
    }
}

}  // namespace
