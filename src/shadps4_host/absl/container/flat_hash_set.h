// Abseil's flat_hash_set for texture_cache.h (the images to download), mapped onto
// std::unordered_set for the reason given in flat_hash_map.h.
#pragma once

#include <unordered_set>

namespace absl {

template <typename Key>
using flat_hash_set = std::unordered_set<Key>;

}  // namespace absl
