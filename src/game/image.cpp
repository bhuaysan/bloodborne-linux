#include "game/image.hpp"

#include <cstdio>
#include <cstdlib>

namespace bb::game {

namespace {
loader::Image* current = nullptr;
}  // namespace

loader::Image& image() {
    if (current == nullptr) {
        std::fputs("game: no image is loaded (bb::game::set_image was not called)\n", stderr);
        std::abort();
    }
    return *current;
}

void set_image(loader::Image* image) {
    current = image;
}

}  // namespace bb::game
