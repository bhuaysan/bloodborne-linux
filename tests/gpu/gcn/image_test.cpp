// Image sampling without a device: the T# class, the image and sampler sources, modules that
// spirv-val accepts for each supported class, and errors that name what isn't supported.

#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "gpu/gcn/gcn.hpp"
#include "gpu/gcn/spirv_val.hpp"
#include "gpu/gcn/translate.hpp"

namespace {

using bb::gcn::GcnError;
using bb::gcn::ImageClass;
using bb::gcn::ResourceSource;
using bb::gcn::ShaderKey;
using bb::gcn::Stage;
using bb::gcn::translate;

// The pixel key: 12 user SGPRs, the perspective barycentrics at the center, mrt0 32_ABGR.
ShaderKey pixel_key(std::vector<ImageClass> images) {
    ShaderKey key{.stage = Stage::pixel, .pgm_rsrc2 = 12u << 1};
    key.ps_input_ena = key.ps_input_addr = 1u << 1;
    key.col_format = 9;
    key.images = std::move(images);
    return key;
}

TEST(GcnImage, ClassOfTsharp) {
    // A 2D array (type 13) of UINT (4), cube (11) of SINT (5), 2D (9) of SRGB (9).
    EXPECT_EQ(bb::gcn::image_class(std::array<std::uint32_t, 4>{0, 4u << 26, 0, 13u << 28}),
              (ImageClass{.type = 13, .number = ImageClass::Number::uint}));
    EXPECT_EQ(bb::gcn::image_class(std::array<std::uint32_t, 4>{0, 5u << 26, 0, 11u << 28}),
              (ImageClass{.type = 11, .number = ImageClass::Number::sint}));
    EXPECT_EQ(bb::gcn::image_class(std::array<std::uint32_t, 4>{~0u, 9u << 26 | 0x3ffffff, ~0u,
                                                                9u << 28 | 0xfffffff}),
              ImageClass{});
}

// A MIMG instruction, `op` with `dmask` and the bits of `flags` in word 0: vdata v10, vaddr v6,
// the T# in s[0:7], the S# in s[8:11]. A program of them ends with s_endpgm.
std::vector<std::uint32_t> mimg(unsigned op, std::uint32_t flags = 0, unsigned dmask = 1) {
    return {0xf0000000 | op << 18 | dmask << 8 | flags, 0x00400a06};
}
std::vector<std::uint32_t> program(std::initializer_list<std::vector<std::uint32_t>> parts) {
    std::vector<std::uint32_t> code;
    for (const auto& p : parts) {
        code.insert(code.end(), p.begin(), p.end());
    }
    code.push_back(0xbf810000);
    return code;
}

TEST(GcnImage, SampleModulesValidate) {
    constexpr std::uint32_t da = 1u << 14;
    // image_sample (all of dmask), _l (da), _d (da, dmask 0) and _lz.
    const auto code = program({mimg(32, 0, 15), mimg(36, da, 3), mimg(34, da, 0), mimg(39, 0, 8)});
    for (const ImageClass c : {ImageClass{}, ImageClass{.type = 13}, ImageClass{.type = 11},
                               ImageClass{.type = 9, .number = ImageClass::Number::uint},
                               ImageClass{.type = 13, .number = ImageClass::Number::sint}}) {
        SCOPED_TRACE(testing::Message() << "type " << int{c.type} << " number "
                                        << static_cast<int>(c.number));
        const auto t = translate(code, {}, pixel_key({c}));
        EXPECT_EQ(t.interface.images, std::vector{ResourceSource{{0}}});
        EXPECT_EQ(t.interface.samplers, std::vector{ResourceSource{{8}}});
        if (bb::test::gcn::have_spirv_val()) {
            EXPECT_EQ(bb::test::gcn::validate(t.spirv), "");
        }
    }
}

std::string error(const std::vector<std::uint32_t>& code, const ShaderKey& key) {
    try {
        translate(code, {}, key);
    } catch (const GcnError& e) {
        const std::string what = e.what();
        return what.substr(0, what.find(" at offset "));
    }
    return "no error";
}

TEST(GcnImage, LoadMipValidates) {
    constexpr std::uint32_t da = 1u << 14, unorm = 1u << 12;
    for (const ImageClass c : {ImageClass{}, ImageClass{.type = 13},
                               ImageClass{.type = 9, .number = ImageClass::Number::uint},
                               ImageClass{.type = 13, .number = ImageClass::Number::sint}}) {
        SCOPED_TRACE(testing::Message() << "type " << int{c.type} << " number "
                                        << static_cast<int>(c.number));
        const auto t = translate(program({mimg(1, unorm, 15), mimg(1, unorm | da, 3)}), {},
                                 pixel_key({c}));
        EXPECT_EQ(t.interface.images, std::vector{ResourceSource{{0}}});
        EXPECT_TRUE(t.interface.samplers.empty());  // an image_load has no sampler
        if (bb::test::gcn::have_spirv_val()) {
            EXPECT_EQ(bb::test::gcn::validate(t.spirv), "");
        }
    }
    // A 2D image reads and ignores the slice with da, like sampling.
    EXPECT_NO_THROW(translate(program({mimg(1, da, 1)}), {}, pixel_key({ImageClass{}})));
}

TEST(GcnImage, ErrorsNameWhatIsUnsupported) {
    const ShaderKey pixel = pixel_key({});
    const ShaderKey compute{.pgm_rsrc2 = 12u << 1};
    EXPECT_EQ(error(program({mimg(39)}), compute), "no error");  // image_sample_lz
    EXPECT_EQ(error(program({mimg(36)}), compute), "no error");  // image_sample_l
    EXPECT_EQ(error(program({mimg(32)}), compute),
              "image_sample (implicit LOD) outside the pixel stage");
    EXPECT_EQ(error(program({mimg(40)}), pixel), "unsupported instruction image_sample_c");
    EXPECT_EQ(error(program({mimg(96)}), pixel), "unsupported instruction image_get_lod");
    EXPECT_EQ(error(program({mimg(0)}), pixel), "unsupported instruction image_load");
    EXPECT_EQ(error(program({mimg(32, 1u << 16)}), pixel),
              "image_sample with tfe, lwe or unorm is not supported yet");
    EXPECT_EQ(error(program({mimg(32, 1u << 12)}), pixel),
              "image_sample with tfe, lwe or unorm is not supported yet");
    EXPECT_EQ(error(program({mimg(32)}), pixel_key({{.type = 10}})),
              "image type 10 of image_sample is not supported yet");
    EXPECT_EQ(error(program({mimg(1, 1u << 17, 1)}), pixel),
              "image_load_mip with tfe or lwe is not supported yet");
    EXPECT_EQ(error(program({mimg(1, 0, 1)}), pixel_key({{.type = 11}})),
              "image type 11 of image_load_mip is not supported yet");
    ShaderKey few = pixel;  // only 8 user SGPRs: the S# can't be followed
    few.pgm_rsrc2 = 8u << 1;
    EXPECT_EQ(error(program({mimg(32)}), few),
              "the image or sampler descriptor of image_sample can't be followed");
}

}  // namespace
