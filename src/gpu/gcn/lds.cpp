// Classifies the DS instructions that the translator handles (lds.hpp).

#include "gpu/gcn/lds.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <string_view>
#include <vector>

namespace bb::gcn::detail {

namespace {

struct AtomicName {
    std::string_view stem;
    DsAtomic op;
    DsAtomic op_signed;  // for the i32 form of min and max
};
constexpr std::array<AtomicName, 14> atomic_names = {{
    {"add", DsAtomic::add, DsAtomic::add},       {"sub", DsAtomic::sub, DsAtomic::sub},
    {"rsub", DsAtomic::rsub, DsAtomic::rsub},    {"inc", DsAtomic::inc, DsAtomic::inc},
    {"dec", DsAtomic::dec, DsAtomic::dec},       {"min", DsAtomic::umin, DsAtomic::smin},
    {"max", DsAtomic::umax, DsAtomic::smax},     {"and", DsAtomic::bit_and, DsAtomic::bit_and},
    {"or", DsAtomic::bit_or, DsAtomic::bit_or},  {"xor", DsAtomic::bit_xor, DsAtomic::bit_xor},
    {"mskor", DsAtomic::mskor, DsAtomic::mskor}, {"wrxchg", DsAtomic::exchange, DsAtomic::exchange},
    {"cmpst", DsAtomic::cmpst, DsAtomic::cmpst},
}};

// The access size in bytes of a type suffix: b8 to b128, i8, u8, i16, u16.
std::optional<std::uint8_t> type_bytes(std::string_view type) {
    unsigned bits = 0;
    if (type.size() < 2 ||
        std::from_chars(type.data() + 1, type.data() + type.size(), bits).ec != std::errc() ||
        (type[0] != 'b' && type[0] != 'i' && type[0] != 'u') ||
        (bits != 8 && bits != 16 && bits != 32 && bits != 64 && bits != 96 && bits != 128)) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(bits / 8);
}

}  // namespace

std::optional<DsOperation> classify_ds(std::string_view name) {
    if (name == "ds_nop") {
        return DsOperation{};
    }
    std::vector<std::string_view> part;
    for (std::size_t from = 0, to; from <= name.size(); from = to + 1) {
        to = std::min(name.find('_', from), name.size());
        part.push_back(name.substr(from, to - from));
    }
    if (part.size() < 3 || part.size() > 4 || part[0] != "ds") {
        return std::nullopt;
    }
    const std::string_view stem = part[1];
    const std::string_view type = part.back();
    const auto bytes = type_bytes(type);
    if (!bytes) {
        return std::nullopt;
    }
    DsOperation d;
    d.bytes = *bytes;
    if (part.size() == 3 && stem.starts_with("read")) {
        // Sizes below a dword are only in the single read, and a pair is only 4 or 8 bytes.
        const bool pair = stem != "read";
        if (pair ? (stem != "read2" && stem != "read2st64") || (*bytes != 4 && *bytes != 8)
                 : (type[0] == 'b' && *bytes < 4)) {
            return std::nullopt;
        }
        d.kind = DsKind::read;
        d.is_signed = type[0] == 'i';
        d.pair = pair;
        d.stride64 = stem == "read2st64";
    } else if (part.size() == 3 && stem.starts_with("write") && type[0] == 'b') {
        const bool pair = stem != "write";
        if (pair && ((stem != "write2" && stem != "write2st64") || (*bytes != 4 && *bytes != 8))) {
            return std::nullopt;
        }
        d.kind = DsKind::write;
        d.pair = pair;
        d.stride64 = stem == "write2st64";
    } else if (*bytes == 4) {
        const bool returns = part.size() == 4 && part[2] == "rtn";
        if (part.size() == 4 && !returns) {
            return std::nullopt;
        }
        const auto* a = std::ranges::find(atomic_names, stem, &AtomicName::stem);
        if (a == atomic_names.end() || type[0] == 'f') {
            return std::nullopt;
        }
        d.kind = DsKind::atomic;
        d.op = type[0] == 'i' ? a->op_signed : a->op;
        d.returns = returns;
        // ds_wrxchg only exists with a return value; the others are what the type letter allows.
        if (stem == "wrxchg" && !returns) {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }
    return d;
}

}  // namespace bb::gcn::detail
