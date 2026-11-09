// The geometry stage that draws a rectangle list (docs/design/graphics.md, "Rectangle lists").
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace bb::gcn {

// A SPIR-V 1.6 geometry shader with the entry point "main". It takes the three vertices of a
// rectangle, as a triangle list, and emits the rectangle as a four-vertex triangle strip. The
// fourth vertex comes from the corner vertex (the one that shares x with one of the others and y
// with the other, in clip space): fourth = a + b - corner, for the position and for every vec4
// output at `locations`. Outputs of the VS that are not listed (clip distances, point size)
// are not forwarded.
std::vector<std::uint32_t> rect_list_geometry(std::span<const std::uint32_t> locations);

}  // namespace bb::gcn
