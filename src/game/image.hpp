// The loaded eboot, for decompiled functions that call or read the game
// (docs/design/runtime.md, "Calling the game").
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

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

// The game's object of type T at link address `link` in image(), e.g. a singleton pointer:
// global<Foo*>(0x5540500).
template <class T>
T& global(std::uint64_t link) {
    return *reinterpret_cast<T*>(image().runtime(link));
}

// Calls the virtual function at byte offset `offset` of `object`'s vtable with `object` as its
// first argument, as the game's `call [vtable + offset]` does.
template <class R, class... A>
R vcall(void* object, std::size_t offset, A... args) {
    void* const* vtable = *static_cast<void* const* const*>(object);
    return reinterpret_cast<R (*)(void*, A...)>(vtable[offset / sizeof(void*)])(object, args...);
}

// The field of type T at byte offset `offset` of a game object, as a reference: for aligned
// fields, e.g. at<void*>(chr, 0x18).
template <class T>
T& at(void* object, std::size_t offset) {
    return *reinterpret_cast<T*>(static_cast<std::byte*>(object) + offset);
}

// The value of type T at byte offset `offset` of a game object, read and written bytewise, so
// any alignment does. T is named at the call, never deduced from the value.
template <class T>
T load(const void* object, std::size_t offset) {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(object) + offset, sizeof value);
    return value;
}

template <class T>
void store(void* object, std::size_t offset, std::type_identity_t<T> value) {
    std::memcpy(static_cast<std::byte*>(object) + offset, &value, sizeof value);
}

}  // namespace bb::game
