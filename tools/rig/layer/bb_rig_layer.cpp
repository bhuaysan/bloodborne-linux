// SPDX-License-Identifier: GPL-2.0-or-later
//
// Vulkan layer of the test rig. It timestamps every vkQueuePresentKHR and, on request, copies the
// presented swapchain image to a file. It is the same for every program the rig runs, so the
// reference and our program are measured in the same way.
//
// BB_RIG_DIR names the directory it works in. Without it the layer only passes calls on.
//   frames.csv   one line per present
//   ctl          FIFO the rig writes commands to: "shot NAME" and "thumb", one per line
//   shots/NAME.raw   header line "BBRIGRAW1 width height vkformat", then the pixels, tightly packed
//   thumb.bin    "seq frame\n", then THUMB_W * THUMB_H luma bytes
// Files appear through rename, so the rig never reads half of one.
//
// A captured frame waits for the copy inside the present call, so its interval and the next one
// are longer than the game's own. frames.csv marks that frame in the `captured` column.
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#define LAYER_EXPORT extern "C" __attribute__((visibility("default")))

namespace {

constexpr int THUMB_W = 64;
constexpr int THUMB_H = 36;

template <typename T>
void* key(T handle) {
    return *reinterpret_cast<void**>(handle);  // the loader's dispatch table pointer
}

struct Instance {
    VkInstance handle = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkDestroyInstance destroy = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties memory_properties = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties queue_families = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR surface_caps = nullptr;
};

struct Swapchain {
    std::vector<VkImage> images;
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    bool capturable = false;
};

struct Device {
    VkDevice handle = VK_NULL_HANDLE;
    Instance* instance = nullptr;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    PFN_vkDestroyDevice destroy = nullptr;
    PFN_vkGetDeviceQueue get_queue = nullptr;
    PFN_vkGetDeviceQueue2 get_queue2 = nullptr;
    PFN_vkCreateSwapchainKHR create_swapchain = nullptr;
    PFN_vkDestroySwapchainKHR destroy_swapchain = nullptr;
    PFN_vkGetSwapchainImagesKHR get_images = nullptr;
    PFN_vkQueuePresentKHR present = nullptr;
    PFN_vkQueueSubmit submit = nullptr;
    PFN_vkCreateCommandPool create_pool = nullptr;
    PFN_vkDestroyCommandPool destroy_pool = nullptr;
    PFN_vkAllocateCommandBuffers alloc_cmd = nullptr;
    PFN_vkBeginCommandBuffer begin_cmd = nullptr;
    PFN_vkEndCommandBuffer end_cmd = nullptr;
    PFN_vkResetCommandBuffer reset_cmd = nullptr;
    PFN_vkCmdPipelineBarrier barrier = nullptr;
    PFN_vkCmdCopyImageToBuffer copy = nullptr;
    PFN_vkCreateBuffer create_buffer = nullptr;
    PFN_vkDestroyBuffer destroy_buffer = nullptr;
    PFN_vkGetBufferMemoryRequirements buffer_reqs = nullptr;
    PFN_vkAllocateMemory alloc_mem = nullptr;
    PFN_vkFreeMemory free_mem = nullptr;
    PFN_vkBindBufferMemory bind = nullptr;
    PFN_vkMapMemory map = nullptr;
    PFN_vkInvalidateMappedMemoryRanges invalidate = nullptr;
    PFN_vkCreateFence create_fence = nullptr;
    PFN_vkDestroyFence destroy_fence = nullptr;
    PFN_vkWaitForFences wait_fence = nullptr;
    PFN_vkResetFences reset_fence = nullptr;

    std::unordered_map<VkSwapchainKHR, Swapchain> swapchains;
    std::unordered_map<VkQueue, uint32_t> queue_family;
    std::unordered_map<uint32_t, VkCommandPool> pools;
    std::unordered_map<uint32_t, VkCommandBuffer> cmds;
    VkFence fence = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize buffer_size = 0;
    void* mapped = nullptr;
    bool mapped_coherent = false;
    bool capture_broken = false;  // a copy never finished, so its command buffer can't be reused
};

std::mutex g_lock;  // guards the maps below and all capture state
std::unordered_map<void*, Instance*> g_instances;
std::unordered_map<void*, Device*> g_devices;
std::unordered_map<VkPhysicalDevice, Instance*> g_gpu_instance;

struct Output {
    std::string dir;
    int frames_fd = -1;
    int ctl_fd = -1;
    std::string ctl_buffer;
    std::vector<std::string> shots;
    bool thumb = false;
    uint64_t frame = 0;
    uint64_t thumb_seq = 0;
    bool active = false;
};
Output g_out;

uint64_t now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000u + uint64_t(ts.tv_nsec);
}

void output_init() {
    static bool done = false;
    if (done) return;
    done = true;
    const char* dir = getenv("BB_RIG_DIR");
    if (!dir || !*dir) return;
    g_out.dir = dir;
    g_out.active = true;
    mkdir((g_out.dir + "/shots").c_str(), 0755);
    g_out.frames_fd = open((g_out.dir + "/frames.csv").c_str(),
                           O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (g_out.frames_fd >= 0) {
        static const char header[] = "frame,present_ns,return_ns,result,width,height,captured\n";
        (void)!write(g_out.frames_fd, header, sizeof(header) - 1);
    }
    // O_RDWR keeps the FIFO open with no writer, so reads return EAGAIN and never EOF.
    g_out.ctl_fd = open((g_out.dir + "/ctl").c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
}

void poll_commands() {
    if (g_out.ctl_fd < 0) return;
    char buf[512];
    ssize_t n;
    while ((n = read(g_out.ctl_fd, buf, sizeof(buf))) > 0) g_out.ctl_buffer.append(buf, size_t(n));
    size_t pos;
    while ((pos = g_out.ctl_buffer.find('\n')) != std::string::npos) {
        std::string line = g_out.ctl_buffer.substr(0, pos);
        g_out.ctl_buffer.erase(0, pos + 1);
        if (line.rfind("shot ", 0) == 0) {
            std::string name = line.substr(5);
            if (name.find('/') == std::string::npos && !name.empty()) g_out.shots.push_back(name);
        } else if (line == "thumb") {
            g_out.thumb = true;
        }
    }
}

bool write_all(int fd, const void* data, size_t size) {
    auto* p = static_cast<const char*>(data);
    while (size > 0) {
        ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        size -= size_t(n);
    }
    return true;
}

void publish(const std::string& path, const std::string& header, const void* data, size_t size) {
    std::string tmp = path + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return;
    bool ok = write_all(fd, header.data(), header.size()) && write_all(fd, data, size);
    close(fd);
    if (ok) {
        rename(tmp.c_str(), path.c_str());
    } else {
        unlink(tmp.c_str());
    }
}

// The formats a capture can copy: 4 bytes per pixel, the size the buffer and the raw file assume.
bool capturable_format(VkFormat format) {
    switch (format) {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
        return true;
    default:
        return false;
    }
}

// Luma of one pixel for the formats swapchains use in practice. Other formats give 0.
int luma(const uint8_t* p, VkFormat format) {
    switch (format) {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        return (p[2] * 54 + p[1] * 183 + p[0] * 19) >> 8;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
        return (p[0] * 54 + p[1] * 183 + p[2] * 19) >> 8;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32: {
        // Only the green channel, which is enough to compare pictures.
        uint32_t v;
        memcpy(&v, p, sizeof(v));
        return int((v >> 12) & 0xFF);
    }
    default:
        return 0;
    }
}

void write_thumb(const uint8_t* pixels, uint32_t w, uint32_t h, VkFormat format) {
    uint8_t thumb[THUMB_W * THUMB_H];
    for (int y = 0; y < THUMB_H; ++y) {
        uint32_t y0 = uint32_t(y) * h / THUMB_H, y1 = std::max(y0 + 1, uint32_t(y + 1) * h / THUMB_H);
        for (int x = 0; x < THUMB_W; ++x) {
            uint32_t x0 = uint32_t(x) * w / THUMB_W;
            uint32_t x1 = std::max(x0 + 1, uint32_t(x + 1) * w / THUMB_W);
            uint64_t sum = 0, count = 0;
            for (uint32_t yy = y0; yy < y1; yy += 2) {
                for (uint32_t xx = x0; xx < x1; xx += 2) {
                    sum += uint64_t(luma(pixels + (uint64_t(yy) * w + xx) * 4, format));
                    ++count;
                }
            }
            thumb[y * THUMB_W + x] = uint8_t(count ? sum / count : 0);
        }
    }
    char header[64];
    snprintf(header, sizeof(header), "%llu %llu\n", (unsigned long long)++g_out.thumb_seq,
             (unsigned long long)g_out.frame);
    publish(g_out.dir + "/thumb.bin", header, thumb, sizeof(thumb));
}

bool ensure_buffer(Device& d, VkDeviceSize size) {
    if (d.buffer != VK_NULL_HANDLE && d.buffer_size >= size) return true;
    if (d.buffer != VK_NULL_HANDLE) {
        d.destroy_buffer(d.handle, d.buffer, nullptr);
        d.free_mem(d.handle, d.memory, nullptr);
        d.buffer = VK_NULL_HANDLE;
        d.memory = VK_NULL_HANDLE;
    }
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (d.create_buffer(d.handle, &info, nullptr, &d.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements reqs{};
    d.buffer_reqs(d.handle, d.buffer, &reqs);
    VkPhysicalDeviceMemoryProperties props{};
    d.instance->memory_properties(d.gpu, &props);
    uint32_t type = UINT32_MAX;
    for (VkMemoryPropertyFlags want :
         {VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_CACHED_BIT),
          VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)}) {
        for (uint32_t i = 0; i < props.memoryTypeCount && type == UINT32_MAX; ++i) {
            if ((reqs.memoryTypeBits & (1u << i)) &&
                (props.memoryTypes[i].propertyFlags & want) == want) {
                type = i;
            }
        }
    }
    if (type == UINT32_MAX) return false;
    d.mapped_coherent = props.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = reqs.size;
    alloc.memoryTypeIndex = type;
    if (d.alloc_mem(d.handle, &alloc, nullptr, &d.memory) != VK_SUCCESS) return false;
    if (d.bind(d.handle, d.buffer, d.memory, 0) != VK_SUCCESS) return false;
    if (d.map(d.handle, d.memory, 0, VK_WHOLE_SIZE, 0, &d.mapped) != VK_SUCCESS) return false;
    d.buffer_size = size;
    return true;
}

bool family_can_copy(Device& d, uint32_t family) {
    uint32_t count = 0;
    d.instance->queue_families(d.gpu, &count, nullptr);
    std::vector<VkQueueFamilyProperties> props(count);
    d.instance->queue_families(d.gpu, &count, props.data());
    return family < count && (props[family].queueFlags & (VK_QUEUE_GRAPHICS_BIT |
                                                          VK_QUEUE_COMPUTE_BIT |
                                                          VK_QUEUE_TRANSFER_BIT));
}

VkCommandBuffer command_buffer(Device& d, uint32_t family) {
    auto found = d.cmds.find(family);
    if (found != d.cmds.end()) return found->second;
    if (!family_can_copy(d, family)) return VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = family;
    VkCommandPool pool;
    if (d.create_pool(d.handle, &pool_info, nullptr, &pool) != VK_SUCCESS) return VK_NULL_HANDLE;
    d.pools[family] = pool;
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd;
    if (d.alloc_cmd(d.handle, &alloc, &cmd) != VK_SUCCESS) return VK_NULL_HANDLE;
    // A layer that creates a dispatchable handle fills in the loader's dispatch pointer itself.
    *reinterpret_cast<void**>(cmd) = key(d.handle);
    d.cmds[family] = cmd;
    return cmd;
}

void image_barrier(Device& d, VkCommandBuffer cmd, VkImage image, VkImageLayout from,
                   VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    d.barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
              nullptr, 0, nullptr, 1, &b);
}

// Copies the image to the host and returns true when its pixels are ready. Once `submitted` is
// set the app's wait semaphores are consumed, so the caller presents without any.
bool capture(Device& d, VkQueue queue, const VkPresentInfoKHR* info, Swapchain& sc, uint32_t index,
             bool& submitted) {
    auto family = d.queue_family.find(queue);
    if (d.capture_broken || family == d.queue_family.end() || !sc.capturable ||
        index >= sc.images.size()) {
        return false;
    }
    const VkDeviceSize size = VkDeviceSize(sc.extent.width) * sc.extent.height * 4;
    if (!ensure_buffer(d, size)) return false;
    VkCommandBuffer cmd = command_buffer(d, family->second);
    if (cmd == VK_NULL_HANDLE) return false;
    if (d.fence == VK_NULL_HANDLE) {
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (d.create_fence(d.handle, &fence_info, nullptr, &d.fence) != VK_SUCCESS) return false;
    }
    d.reset_cmd(cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (d.begin_cmd(cmd, &begin) != VK_SUCCESS) return false;
    VkImage image = sc.images[index];
    image_barrier(d, cmd, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT,
                  VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {sc.extent.width, sc.extent.height, 1};
    d.copy(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d.buffer, 1, &region);
    image_barrier(d, cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_ACCESS_MEMORY_READ_BIT);
    if (d.end_cmd(cmd) != VK_SUCCESS) return false;
    std::vector<VkPipelineStageFlags> stages(info->waitSemaphoreCount,
                                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = info->waitSemaphoreCount;
    submit.pWaitSemaphores = info->pWaitSemaphores;
    submit.pWaitDstStageMask = stages.data();
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    d.reset_fence(d.handle, 1, &d.fence);
    if (d.submit(queue, 1, &submit, d.fence) != VK_SUCCESS) return false;
    submitted = true;
    if (d.wait_fence(d.handle, 1, &d.fence, VK_TRUE, 5000000000ull) != VK_SUCCESS) {
        d.capture_broken = true;
        return false;
    }
    return true;
}

void handle_captured(Device& d, Swapchain& sc) {
    if (!d.mapped_coherent) {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = d.memory;
        range.size = VK_WHOLE_SIZE;
        d.invalidate(d.handle, 1, &range);
    }
    auto* pixels = static_cast<const uint8_t*>(d.mapped);
    const size_t size = size_t(sc.extent.width) * sc.extent.height * 4;
    for (const std::string& name : g_out.shots) {
        char header[96];
        snprintf(header, sizeof(header), "BBRIGRAW1 %u %u %d\n", sc.extent.width, sc.extent.height,
                 int(sc.format));
        publish(g_out.dir + "/shots/" + name + ".raw", header, pixels, size);
    }
    g_out.shots.clear();
    if (g_out.thumb) {
        write_thumb(pixels, sc.extent.width, sc.extent.height, sc.format);
        g_out.thumb = false;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL rig_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info) {
    const uint64_t begin = now_ns();
    Device* device;
    bool captured = false;
    VkPresentInfoKHR patched = *info;
    uint32_t width = 0, height = 0;
    uint64_t frame = 0;
    int frames_fd = -1;
    {
        std::lock_guard lock(g_lock);
        device = g_devices.at(key(queue));
        if (g_out.active && info->swapchainCount > 0) {
            frame = ++g_out.frame;
            frames_fd = g_out.frames_fd;
            poll_commands();
            auto sc = device->swapchains.find(info->pSwapchains[0]);
            if (sc != device->swapchains.end()) {
                width = sc->second.extent.width;
                height = sc->second.extent.height;
                bool submitted = false;
                if ((!g_out.shots.empty() || g_out.thumb) &&
                    capture(*device, queue, info, sc->second, info->pImageIndices[0], submitted)) {
                    handle_captured(*device, sc->second);
                    captured = true;
                }
                if (submitted) {
                    patched.waitSemaphoreCount = 0;
                    patched.pWaitSemaphores = nullptr;
                }
            }
        }
    }
    VkResult result = device->present(queue, &patched);
    if (frames_fd >= 0) {
        char line[160];
        int n = snprintf(line, sizeof(line), "%llu,%llu,%llu,%d,%u,%u,%d\n",
                         (unsigned long long)frame, (unsigned long long)begin,
                         (unsigned long long)now_ns(), int(result), width, height, captured ? 1 : 0);
        (void)!write(frames_fd, line, size_t(n));
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL rig_CreateSwapchainKHR(VkDevice device,
                                                      const VkSwapchainCreateInfoKHR* info,
                                                      const VkAllocationCallbacks* alloc,
                                                      VkSwapchainKHR* out) {
    Device* d;
    bool active;
    {
        std::lock_guard lock(g_lock);
        d = g_devices.at(key(device));
        active = g_out.active;
    }
    VkSurfaceCapabilitiesKHR caps{};
    bool capturable = active && capturable_format(info->imageFormat) &&
                      d->instance->surface_caps &&
                      d->instance->surface_caps(d->gpu, info->surface, &caps) == VK_SUCCESS &&
                      (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkSwapchainCreateInfoKHR patched = *info;
    if (capturable) patched.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkResult result = d->create_swapchain(device, &patched, alloc, out);
    if (result != VK_SUCCESS && capturable) {
        // The surface allows the usage in general, but maybe not with this format or mode.
        capturable = false;
        result = d->create_swapchain(device, info, alloc, out);
    }
    if (result != VK_SUCCESS) return result;
    Swapchain sc;
    sc.extent = info->imageExtent;
    sc.format = info->imageFormat;
    sc.capturable = capturable;
    uint32_t count = 0;
    d->get_images(device, *out, &count, nullptr);
    sc.images.resize(count);
    d->get_images(device, *out, &count, sc.images.data());
    std::lock_guard lock(g_lock);
    d->swapchains[*out] = std::move(sc);
    return result;
}

VKAPI_ATTR void VKAPI_CALL rig_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                   const VkAllocationCallbacks* alloc) {
    Device* d;
    {
        std::lock_guard lock(g_lock);
        d = g_devices.at(key(device));
        d->swapchains.erase(swapchain);
    }
    d->destroy_swapchain(device, swapchain, alloc);
}

VKAPI_ATTR void VKAPI_CALL rig_GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index,
                                              VkQueue* queue) {
    std::lock_guard lock(g_lock);
    Device* d = g_devices.at(key(device));
    d->get_queue(device, family, index, queue);
    d->queue_family[*queue] = family;
}

VKAPI_ATTR void VKAPI_CALL rig_GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info,
                                               VkQueue* queue) {
    std::lock_guard lock(g_lock);
    Device* d = g_devices.at(key(device));
    d->get_queue2(device, info, queue);
    if (*queue != VK_NULL_HANDLE) d->queue_family[*queue] = info->queueFamilyIndex;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL rig_GetDeviceProcAddr(VkDevice device, const char* name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL rig_GetInstanceProcAddr(VkInstance instance,
                                                                 const char* name);

VKAPI_ATTR VkResult VKAPI_CALL rig_CreateDevice(VkPhysicalDevice gpu,
                                                const VkDeviceCreateInfo* info,
                                                const VkAllocationCallbacks* alloc,
                                                VkDevice* out) {
    auto* link = static_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(info->pNext));
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
                     link->function == VK_LAYER_LINK_INFO)) {
        link = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<VkBaseInStructure*>(
            reinterpret_cast<const VkBaseInStructure*>(link)->pNext));
    }
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    Instance* instance;
    {
        std::lock_guard lock(g_lock);
        instance = g_gpu_instance.at(gpu);
    }
    auto create = reinterpret_cast<PFN_vkCreateDevice>(gipa(instance->handle, "vkCreateDevice"));
    VkResult result = create(gpu, info, alloc, out);
    if (result != VK_SUCCESS) return result;

    auto* d = new Device();
    d->handle = *out;
    d->instance = instance;
    d->gpu = gpu;
    d->gdpa = gdpa;
#define LOAD(field, name) d->field = reinterpret_cast<PFN_vk##name>(gdpa(*out, "vk" #name))
    LOAD(destroy, DestroyDevice);
    LOAD(get_queue, GetDeviceQueue);
    LOAD(get_queue2, GetDeviceQueue2);
    LOAD(create_swapchain, CreateSwapchainKHR);
    LOAD(destroy_swapchain, DestroySwapchainKHR);
    LOAD(get_images, GetSwapchainImagesKHR);
    LOAD(present, QueuePresentKHR);
    LOAD(submit, QueueSubmit);
    LOAD(create_pool, CreateCommandPool);
    LOAD(destroy_pool, DestroyCommandPool);
    LOAD(alloc_cmd, AllocateCommandBuffers);
    LOAD(begin_cmd, BeginCommandBuffer);
    LOAD(end_cmd, EndCommandBuffer);
    LOAD(reset_cmd, ResetCommandBuffer);
    LOAD(barrier, CmdPipelineBarrier);
    LOAD(copy, CmdCopyImageToBuffer);
    LOAD(create_buffer, CreateBuffer);
    LOAD(destroy_buffer, DestroyBuffer);
    LOAD(buffer_reqs, GetBufferMemoryRequirements);
    LOAD(alloc_mem, AllocateMemory);
    LOAD(free_mem, FreeMemory);
    LOAD(bind, BindBufferMemory);
    LOAD(map, MapMemory);
    LOAD(invalidate, InvalidateMappedMemoryRanges);
    LOAD(create_fence, CreateFence);
    LOAD(destroy_fence, DestroyFence);
    LOAD(wait_fence, WaitForFences);
    LOAD(reset_fence, ResetFences);
#undef LOAD
    std::lock_guard lock(g_lock);
    output_init();
    g_devices[key(*out)] = d;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL rig_DestroyDevice(VkDevice device, const VkAllocationCallbacks* alloc) {
    Device* d;
    {
        std::lock_guard lock(g_lock);
        auto found = g_devices.find(key(device));
        if (found == g_devices.end()) return;
        d = found->second;
        g_devices.erase(found);
    }
    for (auto& [family, pool] : d->pools) d->destroy_pool(device, pool, nullptr);
    if (d->fence != VK_NULL_HANDLE) d->destroy_fence(device, d->fence, nullptr);
    if (d->buffer != VK_NULL_HANDLE) {
        d->destroy_buffer(device, d->buffer, nullptr);
        d->free_mem(device, d->memory, nullptr);
    }
    d->destroy(device, alloc);
    delete d;
}

VKAPI_ATTR VkResult VKAPI_CALL rig_CreateInstance(const VkInstanceCreateInfo* info,
                                                  const VkAllocationCallbacks* alloc,
                                                  VkInstance* out) {
    auto* link = static_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(info->pNext));
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                     link->function == VK_LAYER_LINK_INFO)) {
        link = reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<VkBaseInStructure*>(
            reinterpret_cast<const VkBaseInStructure*>(link)->pNext));
    }
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    VkResult result = create(info, alloc, out);
    if (result != VK_SUCCESS) return result;

    auto* inst = new Instance();
    inst->handle = *out;
    inst->gipa = gipa;
    inst->destroy = reinterpret_cast<PFN_vkDestroyInstance>(gipa(*out, "vkDestroyInstance"));
    inst->memory_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        gipa(*out, "vkGetPhysicalDeviceMemoryProperties"));
    inst->queue_families = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        gipa(*out, "vkGetPhysicalDeviceQueueFamilyProperties"));
    inst->surface_caps = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
        gipa(*out, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
    auto enumerate =
        reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(*out, "vkEnumeratePhysicalDevices"));
    uint32_t count = 0;
    enumerate(*out, &count, nullptr);
    std::vector<VkPhysicalDevice> gpus(count);
    enumerate(*out, &count, gpus.data());
    std::lock_guard lock(g_lock);
    g_instances[key(*out)] = inst;
    for (VkPhysicalDevice gpu : gpus) g_gpu_instance[gpu] = inst;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL rig_DestroyInstance(VkInstance instance,
                                               const VkAllocationCallbacks* alloc) {
    Instance* inst;
    {
        std::lock_guard lock(g_lock);
        auto found = g_instances.find(key(instance));
        if (found == g_instances.end()) return;
        inst = found->second;
        g_instances.erase(found);
        std::erase_if(g_gpu_instance, [&](const auto& kv) { return kv.second == inst; });
    }
    inst->destroy(instance, alloc);
    delete inst;
}

PFN_vkVoidFunction intercept_device(const char* name) {
#define HOOK(fn)                                                                                   \
    if (strcmp(name, "vk" #fn) == 0) return reinterpret_cast<PFN_vkVoidFunction>(rig_##fn)
    HOOK(GetDeviceProcAddr);
    HOOK(DestroyDevice);
    HOOK(GetDeviceQueue);
    HOOK(GetDeviceQueue2);
    HOOK(CreateSwapchainKHR);
    HOOK(DestroySwapchainKHR);
    HOOK(QueuePresentKHR);
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL rig_GetDeviceProcAddr(VkDevice device, const char* name) {
    if (PFN_vkVoidFunction fn = intercept_device(name)) return fn;
    std::lock_guard lock(g_lock);
    return g_devices.at(key(device))->gdpa(device, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL rig_GetInstanceProcAddr(VkInstance instance,
                                                                 const char* name) {
    HOOK(GetInstanceProcAddr);
    HOOK(CreateInstance);
    HOOK(DestroyInstance);
    HOOK(CreateDevice);
#undef HOOK
    if (PFN_vkVoidFunction fn = intercept_device(name)) return fn;
    if (instance == VK_NULL_HANDLE) return nullptr;
    std::lock_guard lock(g_lock);
    return g_instances.at(key(instance))->gipa(instance, name);
}

}  // namespace

LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* version) {
    if (version->loaderLayerInterfaceVersion < 2) return VK_ERROR_INITIALIZATION_FAILED;
    version->loaderLayerInterfaceVersion = 2;
    version->pfnGetInstanceProcAddr = rig_GetInstanceProcAddr;
    version->pfnGetDeviceProcAddr = rig_GetDeviceProcAddr;
    version->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}
