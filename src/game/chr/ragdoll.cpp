// The characters' ragdolls at the target frame rate (docs/design/frame-rate.md, "The ragdolls
// (#567)").
#include "game/chr/ragdoll.hpp"

#include <immintrin.h>

#include <cstddef>
#include <cstdint>

#include "game/frame/frame.hpp"
#include "game/image.hpp"
#include "hook/hook.hpp"

// Written once by set_ragdoll_patch_targets, before the patches run.
extern "C" {
std::uint64_t bb_ragdoll_impulse_continue = 0;
std::uint64_t bb_ragdoll_root_pull_continue = 0;
}

// Each stub does what the patched bytes do, with the factor bb_step_ratio (1 at 30 FPS, 0.5 at 60)
// applied, and changes the same registers: the impulse stub xmm2, xmm3 and the two frame slots;
// the pull stub xmm0 and xmm1. At 30 they multiply by 1, which leaves every finite value, infinity
// and zero as it is (a signalling NaN would come out quiet).
asm(R"(
    .pushsection .text
    .intel_syntax noprefix
    .globl bb_ragdoll_impulse_stub
    .type bb_ragdoll_impulse_stub, @function
bb_ragdoll_impulse_stub:
    vmovss xmm2, dword ptr [r12 + 0xfc]
    vmulss xmm2, xmm2, dword ptr [rip + bb_step_ratio]
    vmovss dword ptr [rbp - 0x188], xmm2
    vmovss xmm3, dword ptr [r12 + 0x100]
    vmulss xmm3, xmm3, dword ptr [rip + bb_step_ratio]
    vmovss dword ptr [rbp - 0x1d0], xmm3
    jmp qword ptr [rip + bb_ragdoll_impulse_continue]
    .size bb_ragdoll_impulse_stub, . - bb_ragdoll_impulse_stub
    .globl bb_ragdoll_root_pull_stub
    .type bb_ragdoll_root_pull_stub, @function
bb_ragdoll_root_pull_stub:
    vmulss xmm1, xmm1, dword ptr [rip + .Lbb_ragdoll_thirty]
    vmulss xmm1, xmm1, dword ptr [rip + bb_step_ratio]
    vpshufd xmm1, xmm1, 0
    vmulps xmm0, xmm0, xmm1
    jmp qword ptr [rip + bb_ragdoll_root_pull_continue]
    .p2align 2
.Lbb_ragdoll_thirty:
    .float 30.0
    .size bb_ragdoll_root_pull_stub, . - bb_ragdoll_root_pull_stub
    .att_syntax prefix
    .popsection
)");

namespace bb::game {
namespace {

template <class T>
T& at(void* object, std::size_t offset) {
    return *reinterpret_cast<T*>(static_cast<std::byte*>(object) + offset);
}

// The game's 1/30 (0x04926364), 30.0 (0x0492636c) and 900.0 (0x04926370).
constexpr float game_step = 1.0f / 30.0f;
constexpr float update_thirty = 30.0f;
constexpr float idle_nine_hundred = 900.0f;

using RagdollUpdate = void(float step, float unused, void* ragdoll, void* transform, void* pose,
                           std::uint32_t flag);
using RagdollIdle = void(float step, float unused, void* ragdoll, void* transform, void* pose);

}  // namespace

void set_ragdoll_patch_targets() {
    bb_ragdoll_impulse_continue = image().runtime(ragdoll_impulse_continue);
    bb_ragdoll_root_pull_continue = image().runtime(ragdoll_root_pull_continue);
}

// 0x01515ec0
void chr_physics_pose_sync(void* module, void* transform) {
    if (at<void*>(module, 0x20) == nullptr) {
        return;
    }
    // The lock object, read before the character is checked, as the game does.
    void* const physics = at<void*>(at<void*>(module, 0x08), 0x3b0);  // TODO(name): chr+0x3b0
    void* const preferred = at<void*>(physics, 0x30);
    std::byte* lock = nullptr;
    if (preferred != nullptr && at<std::uint8_t>(preferred, 0x384) != 0) {
        lock = at<std::byte*>(preferred, 0x28);
    } else {
        lock = at<std::byte*>(at<void*>(module, 0x10), 0x20);
    }
    if ((at<std::uint8_t>(lock, 0x0c) & 1) != 0) {
        std::int32_t expected = 0;
        if (__atomic_compare_exchange_n(&at<std::int32_t>(lock, 0x10), &expected, 1, false,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            fn<void(void*)>(ragdoll_lock_init)(lock);
        }
    }
    void* const pose = lock + 0x38;  // TODO(name)

    void* const chr = at<void*>(module, 0x08);
    if (chr == nullptr) {
        return;
    }
    const float step = frame_step();  // the game: 1/30 (0x04926364)
    void* const state = at<void*>(chr, 0x18);
    const std::uint8_t flags = at<std::uint8_t>(state, 0x20);
    bool active = (flags & 1) == 0;
    if (active && (flags & 2) != 0) {
        void* const other = at<void*>(state, 0x30);
        active = other != nullptr && (at<std::uint8_t>(other, 0x18) & 1) != 0;
    }
    active = active && at<std::int32_t>(state, 0x10) == 4;
    const std::int32_t interval = active ? at<std::int32_t>(chr, 0xe4) : -1;
    if (interval < 0) {
        fn<RagdollIdle>(ragdoll_update_idle)(step, idle_nine_hundred, at<void*>(module, 0x20),
                                             transform, pose);
        return;
    }
    if (interval > 0) {
        const float spacing = static_cast<float>(interval) * game_step;  // u 30 FPS frames
        const std::uint32_t flag = (at<std::uint32_t>(chr, 0x1e1) & 4) == 0 ? 1 : 0;
        fn<RagdollUpdate>(ragdoll_update_spaced)(step, spacing, at<void*>(module, 0x20), transform,
                                                 pose, flag);
        return;
    }
    // Interval 0: the ragdoll's +0x150 is the character's +0x1e0 with y raised by +0x354.
    void* const ragdoll = at<void*>(module, 0x20);
    void* const body = at<void*>(at<void*>(chr, 0x3b0), 0x68);   // TODO(name)
    const __m128 up = _mm_loadu_ps(&global<float>(0x048f5660));  // {0, 1, 0, 0}
    const __m128 raised = _mm_add_ps(_mm_mul_ps(_mm_set1_ps(at<float>(body, 0x354)), up),
                                     _mm_loadu_ps(&at<float>(body, 0x1e0)));
    _mm_storeu_ps(&at<float>(ragdoll, 0x150), raised);
    at<std::uint8_t>(ragdoll, 0x160) = 1;
    void* const body_now = at<void*>(at<void*>(at<void*>(module, 0x08), 0x3b0), 0x68);
    const std::uint32_t below_one = 1.0f > at<float>(body_now, 0x2f4) ? 1 : 0;
    fn<RagdollUpdate>(ragdoll_update)(step, update_thirty, at<void*>(module, 0x20), transform, pose,
                                      below_one);
}

}  // namespace bb::game

BB_REPLACE(bb::game::chr_physics_pose_sync_link, bb::game::chr_physics_pose_sync);
BB_PATCH(bb::game::ragdoll_update, bb::game::ragdoll_impulse_load, bb_ragdoll_impulse_stub,
         bb::game::ragdoll_impulse_patched);
BB_PATCH(bb::game::ragdoll_update, bb::game::ragdoll_root_pull, bb_ragdoll_root_pull_stub,
         bb::game::ragdoll_root_pull_patched);
