// LeakSanitizer and game memory (docs/design/m3-core-os.md, Memory, "Harness").
#pragma once

#ifdef __SANITIZE_ADDRESS__
#include <sanitizer/lsan_interface.h>
#endif

#include <cstddef>
#include <cstdint>

namespace bb::test {

// Game memory holds pointers to runtime objects (the host mutex behind a guest mutex, say), and
// LeakSanitizer doesn't scan mappings of our own. Registers [start, start + size) as a root
// region, so the objects it points to don't count as leaks. The memory must stay mapped until
// the process exits. Does nothing without AddressSanitizer.
inline void scan_for_leaks(std::uintptr_t start, std::size_t size) {
#ifdef __SANITIZE_ADDRESS__
    __lsan_register_root_region(reinterpret_cast<const void*>(start), size);
#else
    static_cast<void>(start);
    static_cast<void>(size);
#endif
}

}  // namespace bb::test
