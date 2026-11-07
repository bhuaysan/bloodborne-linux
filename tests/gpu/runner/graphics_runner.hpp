// A test-only graphics runner on the compute runner's setup (bbvk's device and scheduler on
// lavapipe), for the render tests of translated VS/PS pairs (docs/design/graphics.md, "Offline
// test strategy", level 3).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "gpu/runner/compute_runner.hpp"

namespace bb::test::vk {

// The image a render goes to: R32G32B32A32_SFLOAT, cleared to `clear`, then copied to the start
// of the guest memory, 16 bytes per pixel, row after row.
struct ColorTarget {
    std::uint32_t width = 0, height = 0;
    std::array<float, 4> clear{};
};

// A sampled image for the pixel shader: R32G32B32A32_SFLOAT, `levels` mip levels of `layers`
// layers each, with a 2D or (`arrayed`) 2D array view. Its texels come from the guest memory at
// byte `offset`: level after level, each layer after layer, 16 bytes per texel, row after row.
struct Texture {
    std::uint32_t width = 0, height = 0, layers = 1, levels = 1;
    bool arrayed = false;
    std::size_t offset = 0;
};

class GraphicsRunner : public ComputeRunner {
public:
    // Nothing when there is no compute runner (ComputeRunner::create); `why` then says why.
    static std::unique_ptr<GraphicsRunner> create(std::string& why);
    ~GraphicsRunner() override;

    // Draws `vertices` vertices as a triangle list without vertex buffers into `target`, with
    // dynamic rendering, no culling, no blending and no depth, and waits for it. `vs` and `ps` are
    // a vertex and a fragment entry point "main" that read one 8-byte push constant, the device
    // address of the draw block: both stages read the same block. The pixel shader's descriptor
    // set 1 holds `textures` at bindings 0 and up, then `samplers` samplers (nearest filtering
    // and mip level, clamp to edge), as the translator binds them. Throws std::logic_error before
    // the first guest_memory() or when the image doesn't fit into it, and std::runtime_error
    // when a Vulkan call fails.
    void render(std::span<const std::uint32_t> vs, std::span<const std::uint32_t> ps,
                const DrawBlock& draw, std::uint32_t vertices, const ColorTarget& target,
                std::span<const Texture> textures = {}, std::uint32_t samplers = 0);

private:
    explicit GraphicsRunner(ComputeRunner&& base);
};

}  // namespace bb::test::vk
