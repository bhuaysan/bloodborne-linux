// Abseil's flat_hash_map for texture_cache.h, which uses it for its samplers and metadata surfaces.
// Abseil is Apache-2.0 and can't be part of this GPL-2.0-or-later program, so the name maps onto
// std::unordered_map. tsl::robin_map would be faster but is not a drop-in: texture_cache.cpp
// assigns through it->second, and robin_map's iterators give const access to the value.
#pragma once

#include <unordered_map>

namespace absl {

template <typename Key, typename Value>
using flat_hash_map = std::unordered_map<Key, Value>;

}  // namespace absl
