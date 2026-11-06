// Tests of the typed buffer format tables (the layouts and which pairs convert). The conversions
// themselves run on lavapipe in tests/gpu/runner/typed_buffer_exec_test.cpp.


#include <gtest/gtest.h>

#include "gpu/gcn/buffer_format.hpp"

namespace {

using namespace bb::gcn;

TEST(GcnBufferFormat, Layouts) {
    const FormatLayout* rgba = format_layout(10);  // 8_8_8_8
    ASSERT_NE(rgba, nullptr);
    EXPECT_EQ(rgba->bytes, 4u);
    EXPECT_EQ(rgba->components, 4u);
    EXPECT_EQ(rgba->field[2].offset, 16);
    EXPECT_EQ(rgba->field[2].width, 8);
    const FormatLayout* packed = format_layout(9);  // 2_10_10_10
    ASSERT_NE(packed, nullptr);
    EXPECT_EQ(packed->field[3].offset, 30);
    EXPECT_EQ(packed->field[3].width, 2);
    const FormatLayout* wide = format_layout(13);  // 32_32_32
    ASSERT_NE(wide, nullptr);
    EXPECT_EQ(wide->bytes, 12u);
    EXPECT_EQ(wide->field[2].word, 2);
}

// Every layout covers its element: the components lie inside its bytes, in order.
TEST(GcnBufferFormat, LayoutsFit) {
    for (unsigned dfmt = 0; dfmt < 16; ++dfmt) {
        const FormatLayout* layout = format_layout(dfmt);
        if (layout == nullptr) {
            continue;
        }
        SCOPED_TRACE(dfmt);
        unsigned bit = 0;
        for (unsigned i = 0; i < layout->components; ++i) {
            const FormatField& f = layout->field[i];
            EXPECT_EQ(f.word * 32 + f.offset, bit);
            bit += f.width;
        }
        EXPECT_LE(bit, 8 * layout->bytes);
    }
}

// No layout for the invalid and reserved formats, the two whose layout is not known, and 10_11_11.
TEST(GcnBufferFormat, MissingLayouts) {
    for (const unsigned dfmt : {0u, 6u, 7u, 8u, 15u, 16u, 99u}) {
        EXPECT_EQ(format_layout(dfmt), nullptr) << dfmt;
        EXPECT_FALSE(format_error(dfmt, nfmt::uint_).empty()) << dfmt;
    }
}

TEST(GcnBufferFormat, NumberFormatsByWidth) {
    EXPECT_TRUE(format_error(1, nfmt::unorm).empty());   // 8
    EXPECT_TRUE(format_error(1, nfmt::sint).empty());
    EXPECT_FALSE(format_error(1, nfmt::float_).empty());  // no float of 8 bits
    EXPECT_FALSE(format_error(9, nfmt::float_).empty());  // nor of 10
    EXPECT_TRUE(format_error(2, nfmt::float_).empty());   // 16
    EXPECT_TRUE(format_error(4, nfmt::float_).empty());   // 32
    EXPECT_TRUE(format_error(4, nfmt::uint_).empty());
    for (const unsigned nf : {nfmt::unorm, nfmt::snorm, nfmt::uscaled, nfmt::sscaled,
                              nfmt::snorm_ogl}) {
        EXPECT_FALSE(format_error(4, nf).empty()) << nf;  // no normalizing of 32 bits
    }
}

// A store takes the number formats that the buffer unit writes (Sea Islands ISA Table 8.13).
TEST(GcnBufferFormat, StoredNumberFormats) {
    for (const unsigned nf : {nfmt::unorm, nfmt::snorm, nfmt::uint_, nfmt::sint}) {
        EXPECT_TRUE(format_error(10, nf, true).empty()) << nf;  // 8_8_8_8
    }
    EXPECT_TRUE(format_error(12, nfmt::float_, true).empty());  // 16_16_16_16
    for (const unsigned nf : {nfmt::uscaled, nfmt::sscaled, nfmt::snorm_ogl}) {
        EXPECT_TRUE(format_error(10, nf).empty()) << nf;
        EXPECT_FALSE(format_error(10, nf, true).empty()) << nf;
    }
    EXPECT_FALSE(format_error(1, nfmt::float_, true).empty());  // the width rules still apply
}

}  // namespace
