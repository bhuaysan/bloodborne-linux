// A test-only compute runner on bbvk's device and scheduler on lavapipe, for the translator's
// execution tests (docs/design/graphics.md, "Per-draw data" and "Offline test strategy", level 3).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include <vulkan/vulkan.h>

#include "gpu/gcn/translate.hpp"

namespace bb::vk {
class Device;
class Scheduler;
}  // namespace bb::vk

namespace bb::test::vk {

// What the execution tests need to know about the device.
struct DeviceInfo {
    std::string name;    // deviceName
    std::string driver;  // driverName and driverInfo
    std::uint32_t subgroup_size = 0;  // in compute, without a required size
    std::uint32_t min_subgroup_size = 0, max_subgroup_size = 0;
    // Float controls for 32-bit floats: denormals preserved or flushed, rounding modes, and
    // signed zero, infinity and NaN preserved.
    bool denorm_preserve_f32 = false, denorm_flush_f32 = false;
    bool round_rte_f32 = false, round_rtz_f32 = false;
    bool signed_zero_inf_nan_f32 = false;
    bool subgroup_quad = false;  // the quad subgroup operations in compute, for s_wqm_b64
    bool quad_control = false;   // bbvk enabled shaderQuadControl (ShaderKey::quad_control)
    bool robust_image_access2 = false;  // bbvk enabled robustImageAccess2

    std::string describe() const;  // one line per property
};

// The layout of the draw block, as the translator reads it.
using bb::gcn::draw_address_bias_offset;
using bb::gcn::draw_block_size;
using bb::gcn::draw_code_address_offset;
using bb::gcn::draw_user_data_offset;

struct DrawBlock {
    std::array<std::uint32_t, 16> user_data{};
    std::uint64_t code_address = 0;  // guest address of the program, for s_getpc_b64
    // address_bias points to a bias table whose entries all hold the guest memory's device
    // address minus its guest address, or to this table instead (the memory mirror's).
    std::optional<std::uint64_t> bias_table;
    // In the runner's table, the entries other than the one the guest memory's base selects hold
    // the bias plus this, so that an access through a wrong entry lands elsewhere.
    std::uint64_t other_entries_offset = 0;
};

class ComputeRunner {
public:
    // The runner on bbvk's device on lavapipe. Nothing when there is no Vulkan 1.3 instance, no
    // lavapipe driver or device, or lavapipe lacks a feature bbvk needs; `why` then says which.
    static std::unique_ptr<ComputeRunner> create(std::string& why);
    virtual ~ComputeRunner();
    ComputeRunner(const ComputeRunner&) = delete;
    ComputeRunner& operator=(const ComputeRunner&) = delete;

    const DeviceInfo& info() const;

    // The guest's GPU-visible memory: one host-visible buffer of `size` bytes at guest address
    // `base`, zeroed, also a transfer destination. It replaces the previous one. The span stays
    // valid until then. Throws std::invalid_argument when `size` is 0.
    std::span<std::byte> guest_memory(std::uint64_t base, std::size_t size);

    // Runs `spirv` (a GLCompute entry point "main" with one 8-byte push constant, the device
    // address of the draw block) on `groups` workgroups and waits for it. Throws
    // std::logic_error before the first guest_memory() (unless `draw` has a bias table) and
    // std::runtime_error when a Vulkan call fails.
    void run(std::span<const std::uint32_t> spirv, const DrawBlock& draw,
             std::array<std::uint32_t, 3> groups);

protected:
    // The setup the graphics runner shares (graphics_runner.hpp).
    ComputeRunner(ComputeRunner&& other) noexcept;
    bb::vk::Device& device();
    bb::vk::Scheduler& scheduler();
    // The guest memory's buffer and size, VK_NULL_HANDLE and 0 before the first guest_memory().
    VkBuffer guest_buffer() const;
    std::size_t guest_size() const;
    // Writes `draw` into the draw block, with the guest memory's bias table unless `draw` has
    // one, and returns the block's device address. Throws std::logic_error before the first
    // guest_memory() when `draw` has no bias table.
    VkDeviceAddress draw_block(const DrawBlock& draw);

private:
    struct State;
    explicit ComputeRunner(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

}  // namespace bb::test::vk
