// Corpus translation report (docs/design/graphics.md, "Offline test strategy", level 2 and
// first-wave item 9): every extracted program is translated with the default key, every module
// that comes out must pass spirv-val, and a report says what the translator can't do yet. The
// report goes to analysis/gcn-translation-coverage.txt. It runs with whatever the translator
// supports: what it rejects is counted, by the instruction it stops at and by its message.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "common/sha256.hpp"
#include "gpu/gcn/corpus.hpp"
#include "gpu/gcn/encode.hpp"
#include "gpu/gcn/gcn.hpp"
#include "gpu/gcn/spirv_val.hpp"
#include "gpu/gcn/translate.hpp"
#include "support/game.hpp"

namespace {

namespace fs = std::filesystem;
using namespace bb::gcn;
using namespace bb::test::gcn;

using Words = std::vector<std::uint32_t>;

constexpr std::size_t max_reports = 20;  // failures reported one by one
constexpr std::uint32_t s_endpgm = sopp(1);

// The key a program of this type is translated with. The footer doesn't carry the register
// values, so these are test defaults. Vertex and copy shaders get VGPR_COMP_CNT 3 and no user
// SGPRs, as in compute. The other programs, whose stages the translator doesn't have yet, get the
// default compute key and are probes: they count the instructions the translator lacks. Domain
// shaders (type 8) run on the VS stage too, but the vertex key can't express their inputs.
ShaderKey default_key(unsigned type) {
    if (type == 1 || type == 6) {
        return {.stage = Stage::vertex, .pgm_rsrc1 = 3u << 24};
    }
    return {};
}

// The message without the position GcnError appends, for grouping.
std::string without_position(const std::string& message) {
    return message.substr(0, message.find(" at offset "));
}

// Messages group by kind: numbers and hex literals become '#', unless a letter or an underscore
// comes before them ("v_add_f32" and "s12" keep theirs).
std::string normalized(const std::string& message) {
    const auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
    std::string out;
    for (std::size_t i = 0; i < message.size();) {
        const bool word = !out.empty() && (std::isalpha(static_cast<unsigned char>(out.back())) ||
                                           out.back() == '_' || is_digit(out.back()));
        const std::size_t start = i;
        if (message.compare(i, 2, "0x") == 0) {
            i += 2;
            while (i < message.size() && std::isxdigit(static_cast<unsigned char>(message[i]))) {
                ++i;
            }
        } else if (is_digit(message[i])) {
            while (i < message.size() && is_digit(message[i])) {
                ++i;
            }
        } else {
            out += message[i++];
            continue;
        }
        if (word) {
            out.append(message, start, i - start);
        } else {
            out += '#';
        }
    }
    return out;
}

// The message of a program or probe by kind: the mnemonic of the instruction it stops at becomes
// "<op>", so that "modifiers of v_add_f32 are not supported yet" is one row.
std::string kind_of(const std::string& message, const std::string& mnemonic) {
    std::string out = message;
    if (!mnemonic.empty()) {
        for (auto at = out.find(mnemonic); at != std::string::npos; at = out.find(mnemonic, at)) {
            out.replace(at, mnemonic.size(), "<op>");
        }
    }
    return normalized(out);
}

// What became of one program (or one probe).
struct Outcome {
    bool translated = false;
    bool wave_size_dependent = false;
    std::string message;  // when it didn't translate (or for the other exception types)
    std::string blocker;  // the mnemonic of the instruction the translator stopped at
    bool internal = false;  // an exception other than GcnError
    std::string invalid;    // spirv-val's messages, when the module is not valid
};

Outcome translate_program(const Words& code, unsigned type, bool validate_module) {
    Outcome out;
    try {
        const auto translation = translate(code, {}, default_key(type));
        out.translated = true;
        out.wave_size_dependent = translation.interface.wave_size_dependent;
        if (validate_module) {
            out.invalid = validate(translation.spirv);
        }
    } catch (const GcnError& e) {
        out.message = without_position(e.what());
        try {
            for (const auto& inst : decode(code, Until::program_end)) {
                if (inst.offset == e.offset()) {
                    out.blocker = std::string(inst.name);
                }
            }
        } catch (const GcnError&) {
        }
    } catch (const std::exception& e) {
        out.internal = true;
        out.message = e.what();
    }
    return out;
}

// One opcode of the corpus: how often the distinct programs use it, and the first instruction
// with it, which the probe translates.
struct OpcodeUse {
    std::string name;
    Format format = Format::sopp;
    std::uint16_t opcode = 0;
    Words first;
    unsigned type = 0;  // the type of the program it is from, for the key of the probe
    std::uint64_t instructions = 0;
    std::uint64_t programs = 0;
};

// Counts by a key, most frequent first.
using Counts = std::map<std::string, std::uint64_t>;

void write_counts(std::ostream& out, const char* title, const char* columns,
                  const Counts& counts) {
    std::vector<std::pair<std::string, std::uint64_t>> rows(counts.begin(), counts.end());
    std::stable_sort(rows.begin(), rows.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });
    out << "\n## " << title << "\n" << columns << "\n";
    for (const auto& [key, n] : rows) {
        out << key << '\t' << n << '\n';
    }
}

TEST(GcnCoverage, EveryShaderTranslatesOrIsCounted) {
    BB_REQUIRE_GAME(game);
    (void)game;
    const fs::path root = bb::test::analysis_dir() / "shaders";
    if (!fs::is_directory(root)) {
        GTEST_SKIP() << root.string() << " is missing: run tools/shaderbnd";
    }
    if (!have_spirv_val()) {
        GTEST_SKIP() << "spirv-val not found";
    }

    // One program file at a time. The code of a program is translated once, however many files
    // hold it; the files are counted per stage.
    std::map<bb::common::Sha256Digest, Outcome> outcomes;
    std::map<bb::common::Sha256Digest, std::string> first_stage;  // of the first file with the code
    std::map<std::pair<int, int>, OpcodeUse> uses;  // by format and opcode, for the probes
    std::map<std::string, std::uint64_t> files_by_stage, translated_by_stage;
    std::uint64_t files = 0, translated_files = 0, wave_files = 0;
    std::size_t failures = 0;
    for (const auto& file : program_files(root)) {
        ++files;
        std::string why;
        const Program program = read_program(file, why);
        if (!why.empty()) {
            ADD_FAILURE() << file.string() << ": " << why;
            continue;
        }
        const auto digest = bb::common::sha256(std::as_bytes(std::span(program.code)));
        auto [it, fresh] = outcomes.try_emplace(digest);
        if (fresh) {
            it->second = translate_program(program.code, program.type, true);
            first_stage[digest] = program_type_name(program.type);
            const Outcome& o = it->second;
            if (o.internal || !o.invalid.empty()) {
                if (++failures <= max_reports) {
                    ADD_FAILURE() << file.string() << ": "
                                  << (o.internal ? "internal error: " + o.message
                                                 : "spirv-val: " + o.invalid);
                }
            }
            std::set<std::pair<int, int>> in_program;
            for (const auto& inst : decode(program.code, Until::program_end)) {
                const std::pair key{int(inst.format), int(inst.opcode)};
                OpcodeUse& use = uses[key];
                if (use.first.empty()) {
                    use.name = inst.name;
                    use.format = inst.format;
                    use.opcode = inst.opcode;
                    use.type = program.type;
                    use.first.assign(program.code.begin() + inst.offset / 4,
                                     program.code.begin() + inst.offset / 4 + inst.size);
                }
                ++use.instructions;
                use.programs += in_program.insert(key).second;
            }
        }
        const std::string stage = program_type_name(program.type);
        ++files_by_stage[stage];
        translated_by_stage[stage] += it->second.translated;
        translated_files += it->second.translated;
        wave_files += it->second.wave_size_dependent;
    }

    // The gaps: programs by the instruction and by the message they stop at, and by the stage
    // (the type of the first file with the code, which picks the key) and the instruction.
    Counts by_blocker, by_message, by_stage_blocker;
    std::uint64_t translated = 0, wave = 0;
    for (const auto& [digest, o] : outcomes) {
        translated += o.translated;
        wave += o.wave_size_dependent;
        if (!o.translated) {
            ++by_message[kind_of(o.message, o.blocker)];
            const std::string blocker = o.blocker.empty() ? "(none)" : o.blocker;
            ++by_blocker[blocker];
            ++by_stage_blocker[first_stage[digest] + '\t' + blocker];
        }
    }

    // The probes: each opcode the corpus uses, alone before an s_endpgm. This lists every opcode
    // the translator lacks, not only the first one of each program. The share of the corpus's
    // instructions whose opcode translates is the headline number. Each probe that translates is
    // validated too, so spirv-val checks real modules even while few whole programs translate.
    struct Probe {
        const OpcodeUse* use;
        std::string result;
    };
    std::vector<Probe> probes;
    Counts probe_messages;
    std::uint64_t instructions = 0, covered = 0;
    for (const auto& [key, use] : uses) {
        Words code = use.first;
        const bool branch = (use.opcode >= 2 && use.opcode <= 9) ||
                            (use.opcode >= 23 && use.opcode <= 26);
        if (use.format == Format::sopp && branch) {
            code[0] &= ~0xffffu;  // a branch to the next instruction, the s_endpgm
        }
        code.push_back(s_endpgm);
        const Outcome o = translate_program(code, use.type, true);
        if (o.internal || !o.invalid.empty()) {
            ADD_FAILURE() << "probe of " << use.name << ": "
                          << (o.internal ? "internal error: " + o.message
                                         : "spirv-val: " + o.invalid);
        }
        probes.push_back({&use, o.translated ? "translates" : kind_of(o.message, o.blocker)});
        ++probe_messages[probes.back().result];
        instructions += use.instructions;
        covered += o.translated ? use.instructions : 0;
    }
    std::stable_sort(probes.begin(), probes.end(), [](const Probe& a, const Probe& b) {
        return a.use->instructions > b.use->instructions;
    });

    std::ofstream out(bb::test::analysis_dir() / "gcn-translation-coverage.txt");
    out << "# Translation coverage of the shader corpus (default key, see coverage_test.cpp)\n"
        << "program files\t" << files << "\n"
        << "distinct programs\t" << outcomes.size() << "\n"
        << "translated programs\t" << translated << "\n"
        << "translated program files\t" << translated_files << "\n"
        << "wave-size-dependent programs\t" << wave << "\n"
        << "wave-size-dependent program files\t" << wave_files << "\n"
        << "opcodes probed\t" << uses.size() << "\n"
        << "opcodes that translate alone\t" << probe_messages["translates"] << "\n"
        << "instructions (distinct programs)\t" << instructions << "\n"
        << "instructions whose opcode translates alone\t" << covered << "\n";
    out << "\n## stages (program files)\nstage\tfiles\ttranslated\n";
    for (const auto& [stage, n] : files_by_stage) {
        out << stage << '\t' << n << '\t' << translated_by_stage[stage] << '\n';
    }
    write_counts(out, "programs by the instruction the translation stops at",
                 "mnemonic\tprograms", by_blocker);
    write_counts(out, "programs by stage and the instruction the translation stops at",
                 "stage\tmnemonic\tprograms", by_stage_blocker);
    write_counts(out, "programs by message", "message\tprograms", by_message);
    write_counts(out, "opcodes by probe result", "result\topcodes", probe_messages);
    out << "\n## probe result per opcode, most used first\n"
        << "mnemonic\tinstructions\tprograms\tresult\n";
    for (const auto& probe : probes) {
        out << probe.use->name << '\t' << probe.use->instructions << '\t' << probe.use->programs
            << '\t' << probe.result << '\n';
    }
    EXPECT_TRUE(out.good());

    EXPECT_EQ(files, 20218u);
    EXPECT_EQ(outcomes.size(), 3832u);
    EXPECT_EQ(failures, 0u) << "every program must translate to a valid module or be counted";
    std::cout << outcomes.size() << " distinct programs, " << translated << " translated, "
              << wave << " wave-size-dependent, " << uses.size() << " opcodes probed\n";
}

TEST(GcnCoverage, ProgramOutcomes) {
    if (!have_spirv_val()) {
        GTEST_SKIP() << "spirv-val not found";
    }
    const Outcome ok = translate_program({s_endpgm}, 0, true);
    EXPECT_TRUE(ok.translated);
    EXPECT_FALSE(ok.internal);
    EXPECT_EQ(ok.invalid, "");
    EXPECT_FALSE(ok.wave_size_dependent);

    // A program that doesn't end is a counted gap with a message, not an internal error.
    const Outcome open = translate_program({sopp(0)}, 0, true);
    EXPECT_FALSE(open.translated);
    EXPECT_FALSE(open.internal);
    EXPECT_FALSE(open.message.empty());
    EXPECT_EQ(open.message.find(" at offset "), std::string::npos);

    // Vertex (1) and copy (6) shaders get the vertex key, so a position export translates. The
    // other types keep the compute key, where exp is an error; domain shaders (8) among them.
    Words position = xport(0xf, 12, 0, 1, 2, 3, xp::done);  // exp pos0 v0, v1, v2, v3 done
    position.push_back(s_endpgm);
    for (const unsigned type : {1u, 6u}) {
        const Outcome vertex = translate_program(position, type, true);
        EXPECT_TRUE(vertex.translated) << "type " << type << ": " << vertex.message;
        EXPECT_EQ(vertex.invalid, "") << "type " << type;
    }
    for (const unsigned type : {0u, 8u}) {
        const Outcome compute = translate_program(position, type, true);
        EXPECT_FALSE(compute.translated) << "type " << type;
        EXPECT_EQ(compute.message, "exp in a compute shader") << "type " << type;
        EXPECT_EQ(compute.blocker, "exp") << "type " << type;
    }
}

TEST(GcnCoverage, KindOfNamesTheInstruction) {
    EXPECT_EQ(kind_of("modifiers of v_add_f32 are not supported yet", "v_add_f32"),
              "modifiers of <op> are not supported yet");
    EXPECT_EQ(kind_of("unsupported instruction exp", ""), "unsupported instruction exp");
}

TEST(GcnCoverage, NormalizedGroupsMessages) {
    EXPECT_EQ(normalized("COMPUTE_PGM_RSRC2 0x1f is not supported"),
              "COMPUTE_PGM_RSRC2 # is not supported");
    EXPECT_EQ(normalized("s12 is used as a mask, 3 times"), "s12 is used as a mask, # times");
    EXPECT_EQ(normalized("unsupported instruction v_add_f32"), "unsupported instruction v_add_f32");
}

}  // namespace
