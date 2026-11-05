// The names of matched NIDs, generated from symbols/nids.csv by tools/nidtable/gen_nid_table.py.
#pragma once

#include <algorithm>
#include <span>
#include <string_view>

namespace bb::hle {

struct NidName {
    std::string_view nid, name;
};

// True if the NIDs are strictly increasing, as the binary search in nid_name() needs.
constexpr bool is_sorted_by_nid(std::span<const NidName> table) {
    return std::ranges::adjacent_find(table, std::ranges::greater_equal{}, &NidName::nid) ==
           table.end();
}

// One entry per NID, sorted by NID.
std::span<const NidName> nid_table();

}  // namespace bb::hle
