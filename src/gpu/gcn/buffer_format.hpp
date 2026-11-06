// Typed buffer formats (MTBUF, docs/design/graphics.md, "Typed buffer formats"): the data and
// number formats of a buffer element, and the SPIR-V that converts between the element's bytes
// and its components.
#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "gpu/gcn/spirv_builder.hpp"

namespace bb::gcn {

// Data formats (the DFMT field) and number formats (NFMT), by their hardware numbers.
namespace nfmt {
inline constexpr unsigned unorm = 0, snorm = 1, uscaled = 2, sscaled = 3, uint_ = 4, sint = 5,
                          snorm_ogl = 6, float_ = 7;
}

// One component of an element: the dword of the element that holds it, and its bit field.
struct FormatField {
    std::uint8_t word = 0, offset = 0, width = 0;
};

struct FormatLayout {
    unsigned bytes = 0;       // the element's size in memory
    unsigned components = 0;  // 1 to 4
    std::array<FormatField, 4> field;
};

// The layout of a data format, nothing for the invalid format, the reserved one, the two formats
// whose layout is not known (10_10_10_2 and 11_11_10) and 10_11_11 (its small floats are not
// done).
const FormatLayout* format_layout(unsigned dfmt);

// Why the pair can't be converted (in the translator's error message), or an empty string.
std::string format_error(unsigned dfmt, unsigned nfmt);

// Emits the conversions. Values are u32 ids whatever their type: floats as their bit patterns.
class FormatEmitter {
public:
    using Id = SpirvBuilder::Id;
    using Words = std::array<Id, 4>;

    FormatEmitter(SpirvBuilder& b, Id u32) : b_(b), u32_(u32) {}

    // The components of an element from its dwords (`raw`; a narrow element sits in the low bits of
    // raw[0]). Components the format doesn't have are 0.
    Words decode(unsigned dfmt, unsigned nfmt, const Words& raw);

private:
    Id float_to_bits(Id value);
    Id extended(unsigned instruction, Id type, const SpirvBuilder::Words& operands);
    Id decode_field(unsigned nfmt, const FormatField& field, Id word);

    SpirvBuilder& b_;
    Id u32_;
};

}  // namespace bb::gcn
