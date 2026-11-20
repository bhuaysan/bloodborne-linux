// Tests of the HUD stage loads in src/game/gfx/hud_stage.cpp (#713): the eboot holds the game's
// loads of the frame buffer size where the list says, set_hud_stage_loads swaps them for loads of
// the stage's size and back, and the stage's bytes, run, give 1920 and 1080.
#include "game/gfx/hud_stage.hpp"

#include <sys/mman.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "game/image.hpp"
#include "harness/eboot.hpp"

namespace {

using bb::game::game_size_load;
using bb::game::hud_size_load_bytes;
using bb::game::hud_size_loads;
using bb::game::HudSizeLoad;
using bb::game::stage_size_load;
using bb::test::EbootTest;

bool holds(const HudSizeLoad& load, const std::array<std::uint8_t, hud_size_load_bytes>& bytes) {
    return std::memcmp(reinterpret_cast<const void*>(bb::game::image().runtime(load.link)),
                       bytes.data(), bytes.size()) == 0;
}

// The permissions ("r-xp" and the like) of the mapping in /proc/self/maps that holds `address`.
std::string mapping_permissions(std::uintptr_t address) {
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        std::istringstream in(line);
        std::uintptr_t start = 0;
        std::uintptr_t end = 0;
        char dash = 0;
        std::string permissions;
        in >> std::hex >> start >> dash >> end >> permissions;
        if (start <= address && address < end) {
            return permissions;
        }
    }
    return "";
}

struct StageGuard {
    ~StageGuard() { bb::game::set_hud_stage_loads(bb::game::image(), 1920, 1080); }
};

TEST(HudStage, GameLoadEncodesTheRipRelativeLoad) {
    // 0x01644357: lea rax, [rip + 0x3ae469a] (0x051289f8); mov eax, [rax]
    EXPECT_EQ(game_size_load(hud_size_loads[0]),
              (std::array<std::uint8_t, 9>{0x48, 0x8d, 0x05, 0x9a, 0x46, 0xae, 0x03, 0x8b, 0x00}));
    // 0x016b3372: lea rcx, [rip + 0x3a7567f] (0x051289f8); mov ecx, [rcx]
    EXPECT_EQ(game_size_load(hud_size_loads[10]),
              (std::array<std::uint8_t, 9>{0x48, 0x8d, 0x0d, 0x7f, 0x56, 0xa7, 0x03, 0x8b, 0x09}));
}

TEST(HudStage, StageLoadsRunToTheStageSize) {
    void* page = ::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(page, MAP_FAILED);
    for (const HudSizeLoad& load : hud_size_loads) {
        auto* code = static_cast<std::uint8_t*>(page);
        ASSERT_EQ(::mprotect(page, 4096, PROT_READ | PROT_WRITE), 0);
        // Fill eax and ecx with garbage first, so the test sees what the load writes.
        const std::uint8_t garbage[] = {0x48, 0xc7, 0xc0, 0xff, 0xff, 0xff, 0xff,   // mov rax, -1
                                        0x48, 0xc7, 0xc1, 0xff, 0xff, 0xff, 0xff};  // mov rcx, -1
        std::memcpy(code, garbage, sizeof garbage);
        code += sizeof garbage;
        const auto bytes = stage_size_load(load);
        std::memcpy(code, bytes.data(), bytes.size());
        code += bytes.size();
        if (load.rcx) {
            const std::uint8_t move[] = {0x48, 0x89, 0xc8};  // mov rax, rcx
            std::memcpy(code, move, sizeof move);
            code += sizeof move;
        }
        *code = 0xc3;  // ret
        ASSERT_EQ(::mprotect(page, 4096, PROT_READ | PROT_EXEC), 0);
        const std::uint64_t value = reinterpret_cast<std::uint64_t (*)()>(page)();
        // The whole register, zero-extended like the game's 32-bit load.
        EXPECT_EQ(value, load.global == bb::game::frame_buffer_width ? 1920U : 1080U)
            << std::hex << load.link;
    }
    ::munmap(page, 4096);
}

TEST_F(EbootTest, HudLoadsHoldTheGamesBytes) {
    int width_loads = 0;
    for (const HudSizeLoad& load : hud_size_loads) {
        EXPECT_TRUE(holds(load, game_size_load(load))) << std::hex << load.link;
        width_loads += load.global == bb::game::frame_buffer_width ? 1 : 0;
    }
    EXPECT_EQ(width_loads, 7);
}

TEST_F(EbootTest, SetHudStageLoadsSwapsTheLoads) {
    StageGuard guard;
    const std::uintptr_t first = image().runtime(hud_size_loads[0].link);
    const std::string before = mapping_permissions(first);
    EXPECT_EQ(before.substr(0, 3), "r-x");
    bb::game::set_hud_stage_loads(image(), 2560, 1440);
    for (const HudSizeLoad& load : hud_size_loads) {
        EXPECT_TRUE(holds(load, stage_size_load(load))) << std::hex << load.link;
    }
    EXPECT_EQ(mapping_permissions(first), before);
    // Twice is the same.
    bb::game::set_hud_stage_loads(image(), 5120, 1440);
    EXPECT_TRUE(holds(hud_size_loads[13], stage_size_load(hud_size_loads[13])));
    // 1080p gives the game's bytes back.
    bb::game::set_hud_stage_loads(image(), 1920, 1080);
    for (const HudSizeLoad& load : hud_size_loads) {
        EXPECT_TRUE(holds(load, game_size_load(load))) << std::hex << load.link;
    }
    EXPECT_EQ(mapping_permissions(first), before);
}

TEST_F(EbootTest, SetHudStageLoadsRefusesUnknownBytesAndWritesNothing) {
    StageGuard guard;
    const HudSizeLoad& last = hud_size_loads.back();
    const std::uint8_t other = 0x90;
    const std::uint8_t game_byte = game_size_load(last)[8];
    bb::game::write_image_bytes(image(), last.link + 8, &other, 1);
    EXPECT_THROW(bb::game::set_hud_stage_loads(image(), 2560, 1440), std::runtime_error);
    EXPECT_TRUE(holds(hud_size_loads[0], game_size_load(hud_size_loads[0])));
    bb::game::write_image_bytes(image(), last.link + 8, &game_byte, 1);
    EXPECT_TRUE(holds(last, game_size_load(last)));
}

}  // namespace
