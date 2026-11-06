// Kernel-internal helpers for the objects that guest cells point to (mutexes, condition variables,
// thread attributes) and for the sce and POSIX entry points of core functions that return a host
// errno (docs/design/m3-core-os.md, "Errors").
#pragma once

#include <cstdint>

#include "hle/kernel/abi.hpp"

namespace bb::hle::kernel {

// A guest cell: a 64-bit word that holds a pointer to one of our objects, or a static initialiser.
using Cell = std::uintptr_t;

// Cell values below this are no pointers to our objects, so they are refused without reading.
inline constexpr Cell lowest_object = 0x10000;

// The object of ours that `value` points to, or nullptr. T has a `tag` member that holds
// T::tag_value while the object lives.
template <class T>
T* object_at(Cell value) {
    if (value < lowest_object || value % alignof(T) != 0) {
        return nullptr;
    }
    T* object = reinterpret_cast<T*>(value);
    return object->tag == T::tag_value ? object : nullptr;
}

// Sce<f>::call and Posix<f>::call wrap a core function `f` in the sce or POSIX error form.
template <auto F>
struct Sce;
template <class... A, int (*F)(A...)>
struct Sce<F> {
    static std::int32_t call(A... args) { return sce_fail(F(args...)); }
};

template <auto F>
struct Posix;
template <class... A, int (*F)(A...)>
struct Posix<F> {
    static int call(A... args) { return pthread_error(F(args...)); }
};

}  // namespace bb::hle::kernel
