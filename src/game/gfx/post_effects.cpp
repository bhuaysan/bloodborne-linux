// bb-run's switches for the game's post-processing effects (#665; docs/design/image-quality.md):
// the wrap of pfx_apply writes them into the block it receives, every frame, since the game builds
// the block again each frame from the map's draw parameters.
#include "game/gfx/post_effects.hpp"

#include <cstring>
#include <format>
#include <stdexcept>

#include "game/image.hpp"
#include "hook/hook.hpp"

namespace bb::game {
namespace {

PostEffects effects;

void check_strength(const char* name, float strength) {
    // The negated test also refuses NaN.
    if (!(strength >= 0.0f && strength <= max_post_effect_strength)) {
        throw std::invalid_argument(
            std::format("{} strength {} is not from 0 to {}", name, strength,
                        max_post_effect_strength));
    }
}

class Editor {
public:
    explicit Editor(std::uint32_t* block) : block_(block) {}

    void set(std::size_t index, std::uint32_t value) {
        edit_.index[edit_.count] = index;
        edit_.saved[edit_.count] = block_[index];
        ++edit_.count;
        block_[index] = value;
    }
    void scale(std::size_t index, float factor) {
        float value = 0;
        std::memcpy(&value, &block_[index], sizeof value);
        value *= factor;
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof bits);
        set(index, bits);
    }
    const PostEffectEdit& edit() const { return edit_; }

private:
    std::uint32_t* block_;
    PostEffectEdit edit_;
};

}  // namespace

void set_post_effects(const PostEffects& e) {
    check_strength("motion blur", e.motion_blur_strength);
    check_strength("chromatic aberration", e.chromatic_aberration_strength);
    effects = e;
}

const PostEffects& post_effects() { return effects; }

PostEffectEdit edit_post_effects(const PostEffects& e, std::uint32_t* block) {
    Editor editor(block);
    if (!e.motion_blur) {
        editor.set(pfx_motion_blur_enable, 0);
    } else if (e.motion_blur_strength != 1.0f) {
        editor.scale(pfx_motion_blur_time_ratio, e.motion_blur_strength);
    }
    if (!e.chromatic_aberration) {
        editor.set(pfx_chromatic_aberration_enable, 0);
    } else if (e.chromatic_aberration_strength != 1.0f) {
        for (std::size_t i = pfx_chromatic_aberration_first; i <= pfx_chromatic_aberration_last;
             ++i) {
            editor.scale(i, e.chromatic_aberration_strength);
        }
    }
    return editor.edit();
}

void restore_post_effects(const PostEffectEdit& edit, std::uint32_t* block) {
    for (std::size_t i = edit.count; i-- > 0;) {
        block[edit.index[i]] = edit.saved[i];
    }
}

void pfx_apply_edited(PfxApply* original, void* self, void* context, std::uint32_t* block) {
    if (block == nullptr || effects == PostEffects{}) {
        original(self, context, block);
        return;
    }
    // The blocks seen in the clinic and Central Yharnam are the frame's copies in its command
    // buffer (docs/design/image-quality.md), which no other thread writes during the call. Were
    // it the live global block, a write of the game thread between the edit and the restore would
    // be undone for one frame; the values are plain scalars, so nothing worse.
    const PostEffectEdit edit = edit_post_effects(effects, block);
    original(self, context, block);
    restore_post_effects(edit, block);
}

// 0x021d5f80
void pfx_apply(void* self, void* context, std::uint32_t* block) {
    pfx_apply_edited(reinterpret_cast<PfxApply*>(hook::original(image(), pfx_apply_link)), self,
                     context, block);
}

}  // namespace bb::game

BB_WRAP(bb::game::pfx_apply_link, bb::game::pfx_apply, bb::game::pfx_apply_prologue);
