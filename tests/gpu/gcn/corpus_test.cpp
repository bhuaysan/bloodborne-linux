// Corpus test (docs/design/graphics.md, "Offline test strategy", level 2): every program that
// tools/shaderbnd extracted from the game decodes completely, ends with its single s_endpgm at
// the size the footer gives, and every distinct instruction in them round-trips through llvm-mc.
// The test also writes an opcode histogram to analysis/ and checks that the game uses nothing
// that Southern Islands lacks.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

#include "common/file.hpp"
#include "common/sha256.hpp"
#include "gpu/gcn/gcn.hpp"
#include "gpu/gcn/llvm_mc.hpp"
#include "support/game.hpp"

namespace {

namespace fs = std::filesystem;
using namespace bb::gcn;
using namespace bb::test::gcn;

using Words = std::vector<std::uint32_t>;

constexpr std::string_view footer_marker = "OrbShdr";
constexpr std::size_t footer_size = 0x1c;
constexpr std::size_t batch_size = 50000;  // distinct instructions per llvm-mc run
constexpr std::size_t max_reports = 20;    // failures reported one by one

// How LLVM's answer to our disassembly differs from the instruction.
enum class Mismatch { rejected, words, text };

struct KnownDifference {
    std::string_view mnemonic;
    Mismatch kind;
    std::string_view reason;
};

// The reviewed list. The differences that docs/design/graphics.md documents per format are
// listed there, and the game hits one of them. The list holds only what the corpus hits, and
// every entry must be hit (a stale entry fails the test).
const std::vector<KnownDifference> known_differences = {
    {"ds_swizzle_b32", Mismatch::text,
     "LLVM prints the offset in its swizzle() form, we print the number, which it accepts and "
     "assembles to the same words"},
};

const char* const format_names[] = {"sop2", "sopk", "sop1",  "sopc",  "sopp", "smrd",
                                    "vop2", "vop1", "vopc",  "vop3",  "vintrp", "ds",
                                    "mubuf", "mtbuf", "mimg", "exp",  "flat"};

// The code of one extracted program (docs/formats/shader-binary.md): the program file holds the
// code, the footer data and the footer block, which says how long the code is. Empty with `why`
// set when the file isn't a program.
Words program_code(const fs::path& file, std::string& why) {
    const auto bytes = bb::common::read_file(file);
    const std::string_view data(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const auto block = data.find(footer_marker);
    if (block == std::string_view::npos || block + footer_size > data.size()) {
        why = "no OrbShdr footer block";
        return {};
    }
    std::uint32_t info = 0;
    std::memcpy(&info, data.data() + block + 8, 4);  // the code size is in bits 8 to 31
    const std::size_t size = info >> 8;
    if (size == 0 || size % 4 != 0 || size > block) {
        why = "the footer's code size " + std::to_string(size) + " doesn't fit the file";
        return {};
    }
    Words code(size / 4);
    std::memcpy(code.data(), data.data(), size);
    return code;
}

// The program files of every extracted archive, in a fixed order.
std::vector<fs::path> program_files(const fs::path& root) {
    std::vector<fs::path> files;
    for (const auto& archive : fs::directory_iterator(root)) {
        if (!fs::is_directory(archive.path() / "programs")) {
            continue;
        }
        for (const auto& entry : fs::recursive_directory_iterator(archive.path() / "programs")) {
            if (entry.is_regular_file()) {
                files.push_back(entry.path());
            }
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

// Every distinct instruction of the corpus, assembled in batches, and what became of it.
class RoundTrip {
public:
    // Adds the instruction at `words`, unless an identical encoding was added before.
    void add(const Instruction& inst, const std::uint32_t* words) {
        const bool fresh =
            inst.size == 1 ? single_.insert(words[0]).second
                           : pairs_.insert(words[0] | std::uint64_t(words[1]) << 32).second;
        if (fresh) {
            lines_.push_back(disassemble(inst));
            originals_.emplace_back(words, words + inst.size);
            formats_.push_back(inst.format);
            if (lines_.size() == batch_size) {
                flush();
            }
        }
    }

    void flush() {
        check(assemble_each(lines_), false);
        check(assemble_each(lines_, "tahiti"), true);
        checked_ += lines_.size();
        lines_.clear();
        originals_.clear();
        formats_.clear();
    }

    std::size_t checked() const { return checked_; }
    // What Southern Islands lacks and how many distinct encodings use it.
    const std::map<std::string, std::size_t>& ci_only() const { return ci_only_; }
    const std::map<std::string_view, std::size_t>& hits() const { return hits_; }

private:
    // Compares the attempts of one GPU with the originals. When Tahiti (Southern Islands) doesn't
    // accept an instruction or gives other words, the game uses a Sea Islands (CI) feature.
    void check(const std::vector<Attempt>& attempts, bool southern_islands) {
        for (std::size_t i = 0; i < lines_.size(); ++i) {
            const auto& [assembled, error] = attempts[i];
            std::string problem;
            Mismatch kind = Mismatch::rejected;
            if (!error.empty()) {
                problem = "llvm-mc rejects it: " + error;
            } else if (assembled.words != originals_[i]) {
                problem = "llvm-mc encodes it differently";
                kind = Mismatch::words;
            } else if (assembled.text != lines_[i] && !southern_islands) {
                problem = "llvm-mc prints it as \"" + assembled.text + "\"";
                kind = Mismatch::text;
            } else {
                continue;
            }
            if (southern_islands) {
                // The 32-bit literal offset of SMRD is new in CI.
                const bool literal_offset =
                    formats_[i] == Format::smrd && originals_[i].size() == 2;
                ++ci_only_[literal_offset ? "SMRD literal offset"
                                          : lines_[i].substr(0, lines_[i].find(' '))];
                continue;
            }
            if (known(lines_[i], kind)) {
                continue;
            }
            if (++problems_ <= max_reports) {
                ADD_FAILURE() << lines_[i] << ": " << problem;
            }
        }
    }

    bool known(const std::string& line, Mismatch kind) {
        const std::string_view mnemonic = std::string_view(line).substr(0, line.find(' '));
        for (const auto& entry : known_differences) {
            if (entry.mnemonic == mnemonic && entry.kind == kind) {
                ++hits_[entry.mnemonic];
                return true;
            }
        }
        return false;
    }

    std::vector<std::string> lines_;
    std::vector<Words> originals_;
    std::vector<Format> formats_;
    std::unordered_set<std::uint32_t> single_;  // the distinct encodings of one dword
    std::unordered_set<std::uint64_t> pairs_;   // of two dwords, the second one in the high half
    std::map<std::string_view, std::size_t> hits_;
    std::map<std::string, std::size_t> ci_only_;
    std::size_t checked_ = 0, problems_ = 0;
};

struct OpcodeCount {
    std::string name;
    std::uint64_t instructions = 0;
    std::uint64_t programs = 0;
};

TEST(GcnCorpus, EveryShaderDecodesAndRoundTrips) {
    BB_REQUIRE_GAME(game);
    (void)game;
    const fs::path root = bb::test::analysis_dir() / "shaders";
    if (!fs::is_directory(root)) {
        GTEST_SKIP() << root.string() << " is missing: run tools/shaderbnd";
    }
    if (!have_llvm_mc()) {
        GTEST_SKIP() << "llvm-mc not found";
    }

    // One program at a time, and a program whose code was seen before is skipped.
    RoundTrip round_trip;
    std::set<bb::common::Sha256Digest> seen;
    std::map<std::pair<int, int>, OpcodeCount> histogram;  // by format and opcode
    std::size_t files = 0, problems = 0;
    std::uint64_t instructions = 0;
    for (const auto& file : program_files(root)) {
        ++files;
        std::string why;
        const Words code = program_code(file, why);
        if (why.empty() && seen.insert(bb::common::sha256(std::as_bytes(std::span(code)))).second) {
            try {
                std::set<std::pair<int, int>> in_program;
                unsigned ends = 0;
                std::uint32_t end = 0;
                const auto insts = decode(code, Until::program_end);
                for (const auto& inst : insts) {
                    end = inst.offset + inst.size * 4;
                    ends += is_endpgm(inst);
                    const std::pair key{int(inst.format), int(inst.opcode)};
                    auto& count = histogram[key];
                    count.name = inst.name;
                    ++count.instructions;
                    count.programs += in_program.insert(key).second;
                    round_trip.add(inst, code.data() + inst.offset / 4);
                }
                instructions += insts.size();
                if (end != code.size() * 4 || ends != 1 || !is_endpgm(insts.back())) {
                    why = "decoded " + std::to_string(end) + " of " +
                          std::to_string(code.size() * 4) + " bytes with " +
                          std::to_string(ends) + " s_endpgm, the last one " +
                          (is_endpgm(insts.back()) ? "" : "not ") + "at the end";
                }
            } catch (const GcnError& e) {
                why = e.what();
            }
        }
        if (!why.empty() && ++problems <= max_reports) {
            ADD_FAILURE() << file.string() << ": " << why;
        }
    }
    round_trip.flush();

    // The extractor's corpus: 20,218 programs, 3,832 of them with different code.
    EXPECT_EQ(files, 20218u);
    EXPECT_EQ(seen.size(), 3832u);
    EXPECT_EQ(problems, 0u);
    // The only feature of Sea Islands (CI) over Southern Islands (SI) that the game uses.
    EXPECT_EQ(round_trip.ci_only(),
              (std::map<std::string, std::size_t>{{"SMRD literal offset", 265}}));
    for (const auto& known : known_differences) {
        EXPECT_GT(round_trip.hits().count(known.mnemonic), 0u) << known.mnemonic << " is stale";
    }

    // The histogram for choosing what to translate first, most used first.
    std::vector<std::pair<std::pair<int, int>, OpcodeCount>> rows(histogram.begin(),
                                                                  histogram.end());
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        return a.second.instructions > b.second.instructions;
    });
    std::ofstream out(bb::test::analysis_dir() / "gcn-opcode-histogram.tsv");
    out << "format\topcode\tname\tinstructions\tprograms\n";
    for (const auto& [key, count] : rows) {
        out << format_names[key.first] << '\t' << key.second << '\t' << count.name << '\t'
            << count.instructions << '\t' << count.programs << '\n';
    }
    EXPECT_TRUE(out.good());
    std::cout << seen.size() << " distinct programs, " << instructions << " instructions, "
              << round_trip.checked() << " distinct encodings, " << rows.size() << " opcodes\n";
}

TEST(GcnCorpus, AssembleEachReportsEveryLine) {
    if (!have_llvm_mc()) {
        GTEST_SKIP() << "llvm-mc not found";
    }
    const std::vector<std::string> lines = {"s_mov_b32 s0, s1", "bogus v0", "s_endpgm",
                                            "flat_load_dword v1, v[2:3]"};
    const auto gfx7 = assemble_each(lines);
    ASSERT_EQ(gfx7.size(), 4u);
    EXPECT_TRUE(gfx7[0].error.empty());
    EXPECT_EQ(gfx7[0].assembled.words, Words{0xbe800301});
    EXPECT_EQ(gfx7[1].error, "invalid instruction");
    EXPECT_EQ(gfx7[2].assembled.words, Words{0xbf810000});
    EXPECT_EQ(gfx7[3].assembled.words, (Words{0xdc300000, 0x01000002}));
    // Southern Islands has no FLAT.
    const auto gfx6 = assemble_each(lines, "tahiti");
    EXPECT_TRUE(gfx6[0].error.empty());
    EXPECT_FALSE(gfx6[1].error.empty());
    EXPECT_EQ(gfx6[2].assembled.words, Words{0xbf810000});
    EXPECT_EQ(gfx6[3].error, "instruction not supported on this GPU");
}

}  // namespace
