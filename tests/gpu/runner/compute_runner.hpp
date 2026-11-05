// A test-only Vulkan 1.3 compute runner on lavapipe, for the translator's execution tests
// (docs/design/graphics.md, "Per-draw data" and "Offline test strategy", level 3).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

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

    std::string describe() const;  // one line per property
};

// The layout of the draw block that the push constant points to, as the shader reads it
// (docs/design/graphics.md, "Per-draw data"): user_data[16], then two 64-bit addresses.
inline constexpr std::uint32_t draw_user_data_offset = 0;
inline constexpr std::uint32_t draw_address_bias_offset = 64;
inline constexpr std::uint32_t draw_code_address_offset = 72;
inline constexpr std::uint32_t draw_block_size = 80;

struct DrawBlock {
    std::array<std::uint32_t, 16> user_data{};
    std::uint64_t code_address = 0;  // guest address of the program, for s_getpc_b64
    // address_bias comes from the guest memory: its device address minus its guest address.
};

class ComputeRunner {
public:
    // The runner on lavapipe. Nothing when there is no Vulkan 1.3 instance, no lavapipe driver
    // or device, or the device lacks a feature the translator needs; `why` then says which.
    static std::unique_ptr<ComputeRunner> create(std::string& why);
    ~ComputeRunner();
    ComputeRunner(const ComputeRunner&) = delete;
    ComputeRunner& operator=(const ComputeRunner&) = delete;

    const DeviceInfo& info() const;

    // The guest's GPU-visible memory: one host-visible buffer of `size` bytes at guest address
    // `base`, zeroed. It replaces the previous one. The span stays valid until then.
    std::span<std::byte> guest_memory(std::uint64_t base, std::size_t size);

    // Runs `spirv` (a GLCompute entry point "main" with one 8-byte push constant, the device
    // address of the draw block) on `groups` workgroups and waits for it. Throws
    // std::runtime_error when a Vulkan call fails.
    void run(std::span<const std::uint32_t> spirv, const DrawBlock& draw,
             std::array<std::uint32_t, 3> groups);

private:
    struct State;
    explicit ComputeRunner(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

}  // namespace bb::test::vk
