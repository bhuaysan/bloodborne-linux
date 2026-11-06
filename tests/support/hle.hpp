// Helpers for tests that call the HLE libraries as the game would.
#pragma once

#include "hle/hle.hpp"
#include "hle/kernel/abi.hpp"
#include "loader/dynamic.hpp"

namespace bb::test {

// The HLE function of `nid` in `library`, resolved as an import of a test module.
template <class F>
F* fn(const char* library, const char* nid) {
    return reinterpret_cast<F*>(
        hle::resolve({nid, library, "libTestModule", loader::stt_func}));
}

// The HLE function of `nid` in libkernel.
template <class F>
F* fn(const char* nid) {
    return fn<F>("libkernel", nid);
}

// The calling thread's guest errno, which is reset to 0.
inline int take_errno() {
    const int e = *hle::kernel::guest_errno_location();
    *hle::kernel::guest_errno_location() = 0;
    return e;
}

// The guest errno for `host_errno`, which must have one.
inline int guest(int host_errno) {
    return hle::kernel::guest_errno(host_errno).value();
}

}  // namespace bb::test
