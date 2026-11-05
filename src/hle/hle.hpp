// The HLE registry, import stubs and data blocks (docs/design/runtime.md, "Imports and stubs").
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "loader/dynamic.hpp"

namespace bb::hle {

// One stubbed function import. Lives as long as the process; its thunk passes its address.
struct StubInfo {
    std::string library, module, nid;
    std::string_view name;                         // "?" until the NID name table exists
    mutable std::atomic<std::uint64_t> calls{0};  // calls so far, including the current one
};

// Receives stub calls instead of the log while it is installed.
class StubObserver {
public:
    virtual ~StubObserver() = default;
    virtual void on_call(const StubInfo& stub, const std::uint64_t args[6]) = 0;
};

// Registers `impl` for an import. Throws std::logic_error if the import already has an
// implementation or was already resolved.
void register_function(std::string_view library, std::string_view nid, void* impl);

// The registered implementation of `imp`, else its function stub (STT_FUNC) or its zero-filled
// 4 KiB data block (STT_OBJECT). Created on the first call per (library, NID), then cached.
std::uintptr_t resolve(const loader::Import& imp);

// What resolve() returned for this import. Aborts if it was never resolved.
std::uintptr_t import_address(std::string_view library, std::string_view nid);

// nullptr restores logging. The observer must outlive its installation.
void set_stub_observer(StubObserver* observer);

// Called by stub_entry (stub_entry.S, through the mangled name) with the integer argument
// registers rdi, rsi, rdx, rcx, r8, r9.
void on_stub_call(const StubInfo* stub, const std::uint64_t args[6]);

// Whether call number `call` (counted from 1) of one stub is logged: 1 to 10, then every 1000th.
constexpr bool should_log(std::uint64_t call) {
    return call <= 10 || call % 1000 == 0;
}

// The numbers of function stubs and data blocks that resolve() created so far.
struct Counts {
    std::size_t stubs, data_blocks;
};
Counts counts();

// Registers a function at static initialization (docs/design/runtime.md, "Static
// registration"). Use BB_HLE_FUNCTION.
struct Registration {
    Registration(std::string_view library, std::string_view nid, void* impl) {
        register_function(library, nid, impl);
    }
};

}  // namespace bb::hle

#define BB_HLE_CONCAT_(a, b) a##b
#define BB_HLE_CONCAT(a, b) BB_HLE_CONCAT_(a, b)

// BB_HLE_FUNCTION("libkernel", "AAAAAAAAAAA", my_function); at namespace scope in a source file
// of an OBJECT library.
#define BB_HLE_FUNCTION(library, nid, function)                                         \
    [[maybe_unused]] static const ::bb::hle::Registration BB_HLE_CONCAT(bb_hle_reg_, __LINE__) { \
        library, nid, reinterpret_cast<void*>(&function)                                \
    }
