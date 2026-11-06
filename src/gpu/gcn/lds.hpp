// The DS instructions that the translator turns into accesses of the workgroup's LDS (internal to
// bbgcn). Semantics from the Sea Islands ISA manual, chapter "Data Share (DS) Instructions".
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace bb::gcn::detail {

enum class DsKind : std::uint8_t {
    nop,
    read,    // ds_read_*, ds_read2_*, ds_read2st64_*
    write,   // ds_write_*, ds_write2_*, ds_write2st64_*
    atomic,  // read-modify-write of one dword at the address
};

enum class DsAtomic : std::uint8_t {
    add, sub, rsub, inc, dec, smin, smax, umin, umax, bit_and, bit_or, bit_xor, mskor, exchange,
    cmpst,
};

struct DsOperation {
    DsKind kind = DsKind::nop;
    // read and write: the size of one access in bytes (1, 2, 4, 8, 12 or 16), and whether a read
    // narrower than a dword sign-extends. read2 and write2 make two accesses (`pair`), at
    // offset0 and offset1 times the access size, times 64 for the st64 forms.
    std::uint8_t bytes = 4;
    bool is_signed = false;
    bool pair = false;
    bool stride64 = false;
    // atomic
    DsAtomic op = DsAtomic::add;
    bool returns = false;
};

// What the translator does for the named DS instruction (LLVM's mnemonic), nothing for the ones it
// doesn't handle: the 64-bit and float atomics, the src2 forms, GWS, append and consume.
// ds_swizzle_b32 is no LDS access: the translator handles it apart from this table.
std::optional<DsOperation> classify_ds(std::string_view name);

}  // namespace bb::gcn::detail
