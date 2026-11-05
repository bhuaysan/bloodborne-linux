// The loaded eboot, for decompiled functions that call or read the game
// (docs/design/runtime.md, "Calling the game").
#pragma once

#include <cstdint>

#include "loader/image.hpp"

namespace bb::game {

// The image set with set_image. Aborts with a message when none is set.
loader::Image& image();

// Sets the image that image() returns, nullptr to unset it. Called by the game runtime and the
// harness after loading. The image must outlive its use through image().
void set_image(loader::Image* image);

// The game function at link address `link` in image(), e.g. fn<void(Foo*)>(0x123450)(foo).
// Throws std::out_of_range unless `link` lies inside an executable segment.
template <class F>
F* fn(std::uint64_t link) {
    return image().fn<F>(link);
}

}  // namespace bb::game
