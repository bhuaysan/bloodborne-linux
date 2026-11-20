// A black backdrop for the loading page at a frame buffer wider than 16:9 (#711).
#include "game/gfx/loading_page.hpp"

#include "game/camera/aspect.hpp"
#include "game/image.hpp"
#include "hook/hook.hpp"

namespace bb::game {

bool wants_black_backdrop(std::u16string_view name, float aspect) {
    return aspect != game_aspect && name == loading_page_movie;
}

namespace {

using MovieHolderConstruct = void(void* holder, void* owner, void* definition, std::uint64_t kind,
                                  const char16_t* name, bool start);
using SetBackground = void(void* movie, const std::uint32_t* color);

// 0x01f58360
void movie_holder_construct_backdrop(void* holder, void* owner, void* definition,
                                     std::uint64_t kind, const char16_t* name, bool start) {
    reinterpret_cast<MovieHolderConstruct*>(hook::original(image(), movie_holder_construct))(
        holder, owner, definition, kind, name, start);
    void* const movie = load<void*>(holder, movie_holder_movie);
    if (movie == nullptr || name == nullptr ||
        !wants_black_backdrop(std::u16string_view(name), screen_aspect())) {
        return;
    }
    const auto* const vtable = load<const std::uintptr_t*>(movie, 0);
    const std::uint32_t color = opaque_black;
    reinterpret_cast<SetBackground*>(vtable[movie_set_background_slot / 8])(movie, &color);
}

}  // namespace
}  // namespace bb::game

BB_WRAP(bb::game::movie_holder_construct, bb::game::movie_holder_construct_backdrop,
        bb::game::movie_holder_construct_prologue);
