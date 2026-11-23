// What bb-run hands the renderer from its options (src/tools/renderer_options.hpp, #725): each
// option reaches its field.
#include "tools/renderer_options.hpp"

#include <gtest/gtest.h>

namespace {

using bb::tools::PresentMode;
using bb::tools::RunOptions;

TEST(RendererOptions, TheDefaultsAreTheRenderersDefaults) {
    const Vulkan::RendererOptions r = bb::tools::renderer_options(RunOptions{}, -1);
    const Vulkan::RendererOptions d;
    EXPECT_EQ(r.gpu_id, d.gpu_id);
    EXPECT_EQ(r.validation.enable, d.validation.enable);
    EXPECT_EQ(r.validation.sync, d.validation.sync);
    EXPECT_EQ(r.validation.gpu_assisted, d.validation.gpu_assisted);
    EXPECT_EQ(r.present_mode, d.present_mode);
    EXPECT_EQ(r.fsr, d.fsr);
    EXPECT_EQ(r.rcas, d.rcas);
    EXPECT_EQ(r.rcas_attenuation, d.rcas_attenuation);
    EXPECT_EQ(r.anisotropy, d.anisotropy);
}

TEST(RendererOptions, EachOptionReachesItsField) {
    RunOptions o;
    o.validation = true;
    o.validation_gpu = true;
    o.present_mode = PresentMode::immediate;
    o.fsr = true;
    o.rcas = false;
    o.rcas_attenuation = 1.5f;
    o.anisotropy = 8;
    Vulkan::RendererOptions r = bb::tools::renderer_options(o, 2);
    EXPECT_EQ(r.gpu_id, 2);
    EXPECT_TRUE(r.validation.enable);
    EXPECT_FALSE(r.validation.sync);
    EXPECT_TRUE(r.validation.gpu_assisted);
    EXPECT_EQ(r.present_mode, Vulkan::PresentMode::Immediate);
    EXPECT_TRUE(r.fsr);
    EXPECT_FALSE(r.rcas);
    EXPECT_EQ(r.rcas_attenuation, 1.5f);
    EXPECT_EQ(r.anisotropy, 8U);

    o.validation_gpu = false;
    o.validation_sync = true;
    o.present_mode = PresentMode::fifo;
    o.fsr = false;
    o.rcas = true;
    r = bb::tools::renderer_options(o, 0);
    EXPECT_TRUE(r.validation.sync);
    EXPECT_FALSE(r.validation.gpu_assisted);
    EXPECT_EQ(r.present_mode, Vulkan::PresentMode::Fifo);
    EXPECT_FALSE(r.fsr);
    EXPECT_TRUE(r.rcas);
    o.present_mode = PresentMode::mailbox;
    EXPECT_EQ(bb::tools::renderer_options(o, 0).present_mode, Vulkan::PresentMode::Mailbox);
}

}  // namespace
