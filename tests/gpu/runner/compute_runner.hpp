// A test-only Vulkan 1.3 compute runner on lavapipe, for the translator's execution tests
// (docs/design/graphics.md, "Offline test strategy", level 3).
#pragma once

#include <cstdint>
#include <memory>
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

class ComputeRunner {
public:
    // The runner on lavapipe. Nothing when there is no Vulkan 1.3 instance, no lavapipe driver
    // or device, or the device lacks a feature the translator needs; `why` then says which.
    static std::unique_ptr<ComputeRunner> create(std::string& why);
    ~ComputeRunner();
    ComputeRunner(const ComputeRunner&) = delete;
    ComputeRunner& operator=(const ComputeRunner&) = delete;

    const DeviceInfo& info() const;

private:
    struct State;
    explicit ComputeRunner(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

}  // namespace bb::test::vk
