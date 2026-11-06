// bbvk's device core: the Vulkan instance, the choice of the physical device, the features the
// backend and the translator need, and the queues (docs/design/graphics.md, "Command buffers to
// the first frame", item 14).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <vulkan/vulkan.h>

namespace bb::vk {

// A Vulkan structure with its type and next pointer set and everything else zero.
template <typename T>
T make(VkStructureType type, void* next = nullptr) {
    T value{};
    value.sType = type;
    value.pNext = next;
    return value;
}

// Throws std::runtime_error naming `call` and the result unless `result` is VK_SUCCESS.
void check(VkResult result, const char* call);

class Instance {
public:
    // A Vulkan 1.3 instance. Nothing when the loader makes none; `why` then gives the VkResult
    // of vkCreateInstance.
    static std::unique_ptr<Instance> create(std::string& why);
    ~Instance();
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;

    VkInstance handle() const { return instance_; }

private:
    explicit Instance(VkInstance instance) : instance_(instance) {}
    VkInstance instance_;
};

// A physical device as the choice sees it.
struct Candidate {
    std::string name;  // deviceName
    VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    std::optional<std::string> missing;  // what the device lacks, or nothing when it has it all
};

// The index of the device to use. With an empty selector, the first suitable device by type
// (discrete, integrated, virtual, CPU, other), in enumeration order within a type. Otherwise the
// same among the devices whose name contains the selector, ignoring case. Nothing when no device
// fits; `why` then says why.
std::optional<std::size_t> choose_device(std::span<const Candidate> candidates,
                                         std::string_view selector, std::string& why);

struct DeviceOptions {
    // The device selector of choose_device; empty for the automatic choice. Tests pass
    // "llvmpipe" for lavapipe.
    std::string device;
};

// What the backend reads about the chosen device. The pNext members are null.
struct DeviceProperties {
    VkPhysicalDeviceProperties core{};
    VkPhysicalDeviceVulkan11Properties v11{};
    VkPhysicalDeviceVulkan12Properties v12{};
    VkPhysicalDeviceVulkan13Properties v13{};
    VkDeviceSize min_imported_host_pointer_alignment = 0;  // VK_EXT_external_memory_host
};

// One VkQueue. Vulkan wants calls on a queue externally synchronized: hold `lock` around them.
struct Queue {
    VkQueue handle = VK_NULL_HANDLE;
    std::uint32_t family = 0;
    std::mutex lock;
};

class Device {
public:
    // Chooses the physical device (choose_device over the devices of `instance`; a suitable one
    // has what docs/design/graphics.md, "bbvk device core", requires) and creates the logical
    // device with the required features enabled and no others. Nothing when no device fits;
    // `why` then says why. Throws std::runtime_error when a Vulkan call fails. The instance must
    // outlive the device.
    static std::unique_ptr<Device> create(const Instance& instance, const DeviceOptions& options,
                                          std::string& why);
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    VkPhysicalDevice physical() const { return physical_; }
    VkDevice handle() const { return device_; }
    const DeviceProperties& properties() const { return properties_; }

    // The queue for graphics, compute and transfers: the first family with graphics and compute.
    Queue& graphics() { return graphics_; }
    // The queue for sparse binding: the first family with nothing but sparse binding (RADV has
    // one), otherwise the graphics queue itself.
    Queue& sparse() { return sparse_ ? *sparse_ : graphics_; }

private:
    Device() = default;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    DeviceProperties properties_;
    Queue graphics_;
    std::optional<Queue> sparse_;
};

}  // namespace bb::vk
