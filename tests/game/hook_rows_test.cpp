// Checks symbols/functions.csv against the hooks that bbgame registers (#734): every hooked game
// function has a row with status `replaced`, and every `replaced` row is hooked and names its
// address in its file. Needs no eboot.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "game/gfx/hud_stage.hpp"
#include "hook/hook.hpp"

namespace {

namespace fs = std::filesystem;

const fs::path source_dir{BB_SOURCE_DIR};

// Functions the game changes by byte writes alone, without a registered hook: the HUD loads of
// src/game/gfx/hud_stage.hpp (hud_size_loads), in the functions its comment names.
const std::map<std::uint64_t, std::string> byte_write_functions = {
    {0x016439b0, "hud_world_markers_project"}, {0x016448f0, "lock_on_reticle_project"},
    {0x01644cf0, "enemy_bars_project"},        {0x01654d80, "enemy_bar_update"},
    {0x016b3320, "lock_on_reticle_visible"},   {0x01bfb2a0, "hud_update"},
};

// The farthest a HUD load lies behind the entry of its function in byte_write_functions.
constexpr std::uint64_t max_load_offset = 0x4000;

struct Row {
    std::string name;
    std::string status;
    std::string file;
};

// The fields of one CSV line, with double-quoted fields ("" for a quote inside one).
std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> fields(1);
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') {
                fields.back() += '"';
                ++i;
            } else if (c == '"') {
                quoted = false;
            } else {
                fields.back() += c;
            }
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',') {
            fields.emplace_back();
        } else {
            fields.back() += c;
        }
    }
    return fields;
}

// symbols/functions.csv by address.
std::map<std::uint64_t, Row> read_rows() {
    std::ifstream in(source_dir / "symbols" / "functions.csv");
    EXPECT_TRUE(in) << "cannot open symbols/functions.csv under " << source_dir;
    std::map<std::uint64_t, Row> rows;
    std::string line;
    std::getline(in, line);
    EXPECT_EQ(line, "address,name,signature,status,file");
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        const std::vector<std::string> f = split_csv(line);
        if (f.size() != 5) {
            ADD_FAILURE() << "functions.csv row without 5 fields: " << line;
            continue;
        }
        const std::uint64_t addr = std::stoull(f[0], nullptr, 16);
        if (!rows.emplace(addr, Row{f[1], f[3], f[4]}).second) {
            ADD_FAILURE() << "functions.csv lists " << f[0] << " twice";
        }
    }
    return rows;
}

std::string hex(std::uint64_t addr) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "0x%08llx", static_cast<unsigned long long>(addr));
    return buf;
}

std::string lower_file(const fs::path& path) {
    std::ifstream in(path);
    std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// The game functions the registered hooks change: a BB_PATCH counts as its function.
std::map<std::uint64_t, std::string> hooked_functions() {
    std::map<std::uint64_t, std::string> hooked;
    for (const bb::hook::Replacement& r : bb::hook::replacements()) {
        hooked.emplace(r.function != 0 ? r.function : r.link_addr, r.name);
    }
    return hooked;
}

TEST(HookRows, RegistryIsNotEmpty) {
    // bbgame registers its hooks during static initialization; an empty registry would make the
    // other tests pass for nothing.
    EXPECT_GE(bb::hook::replacements().size(), 20U);
}

TEST(HookRows, EveryHookedFunctionHasAReplacedRow) {
    const auto rows = read_rows();
    for (const auto& [addr, name] : hooked_functions()) {
        const auto it = rows.find(addr);
        if (it == rows.end()) {
            ADD_FAILURE() << name << " hooks " << hex(addr)
                          << ", which has no row in functions.csv";
        } else {
            EXPECT_EQ(it->second.status, "replaced")
                << name << " hooks " << hex(addr) << " (" << it->second.name << ")";
        }
    }
    for (const auto& [addr, name] : byte_write_functions) {
        const auto it = rows.find(addr);
        if (it == rows.end()) {
            ADD_FAILURE() << name << " " << hex(addr) << " has no row in functions.csv";
        } else {
            EXPECT_EQ(it->second.status, "replaced") << name << " " << hex(addr);
        }
    }
}

TEST(HookRows, EveryReplacedRowIsHookedAndNamedInItsFile) {
    const auto hooked = hooked_functions();
    for (const auto& [addr, row] : read_rows()) {
        if (row.status != "replaced") {
            continue;
        }
        EXPECT_TRUE(hooked.contains(addr) || byte_write_functions.contains(addr))
            << row.name << " " << hex(addr) << " is replaced, but nothing hooks it";
        const fs::path file = source_dir / row.file;
        if (row.file.empty() || !fs::is_regular_file(file)) {
            ADD_FAILURE() << row.name << " " << hex(addr) << ": file '" << row.file
                          << "' does not exist";
            continue;
        }
        // The address may stand in the file or in its header (named link addresses).
        fs::path header = file;
        header.replace_extension(".hpp");
        std::string text = lower_file(file);
        if (fs::is_regular_file(header)) {
            text += lower_file(header);
        }
        EXPECT_NE(text.find(hex(addr)), std::string::npos)
            << row.name << ": " << row.file << " does not name " << hex(addr);
    }
}

// The byte-write list stays in step with hud_size_loads: every load lies in a listed function (the
// nearest listed entry before it, within max_load_offset), and every listed function has a load.
TEST(HookRows, ByteWriteListMatchesHudLoads) {
    std::set<std::uint64_t> with_load;
    for (const bb::game::HudSizeLoad& load : bb::game::hud_size_loads) {
        auto it = byte_write_functions.upper_bound(load.link);
        if (it == byte_write_functions.begin()) {
            ADD_FAILURE() << "HUD load " << hex(load.link) << " lies before every listed function";
            continue;
        }
        --it;
        if (load.link - it->first >= max_load_offset) {
            ADD_FAILURE() << "HUD load " << hex(load.link) << " lies in no listed function";
            continue;
        }
        with_load.insert(it->first);
    }
    for (const auto& [addr, name] : byte_write_functions) {
        EXPECT_TRUE(with_load.contains(addr)) << name << " " << hex(addr) << " has no HUD load";
    }
}

}  // namespace
