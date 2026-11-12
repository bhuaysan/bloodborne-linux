// video_core's presenter behind video out and the GNM driver (docs/design/renderer.md, "Startup
// order"): Liverpool, whose GPU thread starts with it, then the Presenter with its instance,
// schedulers, swapchain and rasterizer, as upstream's GNM driver creates them. bb-run makes one
// for its window, or with --headless one without a window, and hands it to start_video_out and
// start_gnm. Without a window there is Liverpool only: it runs the command buffers without a
// rasterizer, so nothing is drawn, and flips complete without presenting.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "hle/video/gnm_driver.hpp"
#include "hle/video/video_out.hpp"

namespace AmdGpu {
class Liverpool;
}
namespace Frontend {
class WindowSDL;
enum class WindowSystemType : std::uint8_t;
}
namespace Vulkan {
class Presenter;
}

namespace bb::hle::video {

class VideoCorePresenter final : public FlipPresenter, public CommandProcessor {
public:
    // Creates Liverpool and, with a `window`, the Presenter on it, on the Vulkan device of
    // GetGpuId(). Fails as upstream does (an assertion) when Vulkan can't.
    explicit VideoCorePresenter(Frontend::WindowSDL* window);
    ~VideoCorePresenter() override;

    void set_vo_port(VideoOutPort* port) override;
    void send_command(std::function<void()> work) override;
    void register_surface(const BufferAttributeGroup& group, std::uint64_t address) override;
    Vulkan::Frame* prepare_frame(const BufferAttributeGroup& group,
                                 std::uint64_t address) override;
    Vulkan::Frame* prepare_blank_frame(bool present_thread) override;
    Vulkan::Frame* prepare_last_frame() override;
    void present(Vulkan::Frame* frame, bool is_reusing_frame, bool is_game_frame) override;
    void set_hdr(bool enable) override;
    bool must_keep_drawing() override;

    void submit_gfx(std::span<const std::uint32_t> dcb,
                    std::span<const std::uint32_t> ccb) override;
    void submit_asc(std::uint32_t gnm_vqid, std::span<const std::uint32_t> acb) override;
    void submit_done() override;
    bool is_gpu_idle() override;
    std::uint32_t map_queue(std::uint32_t* ring, std::uint32_t ring_dwords,
                            std::uint32_t* read_pointer, std::uint32_t pipe) override;
    bool unmap_queue(std::uint32_t index) override;
    std::span<const std::uint32_t> queue_ring(std::uint32_t index) override;

    // The number of presents of the game's frames so far.
    std::uint64_t game_frames() const { return game_frames_; }
    // The rasterizer's draws and dispatches so far (GpuStats' counters).
    static std::uint64_t draws();
    static std::uint64_t dispatches();

private:
    std::unique_ptr<AmdGpu::Liverpool> liverpool_;
    std::unique_ptr<Vulkan::Presenter> presenter_;
    std::atomic<std::uint64_t> game_frames_{0};
};

// The index GetGpuId() takes for the --device selector: the first Vulkan device whose name
// contains `selector` ignoring case, in the order in which the Presenter's instance on a window of
// `window_type` enumerates them; -1 (the instance's own choice) for an empty selector. Nothing
// when no device matches; `why` then lists the names.
std::optional<std::int32_t> resolve_gpu_id(std::string_view selector,
                                          Frontend::WindowSystemType window_type,
                                          std::string& why);

}  // namespace bb::hle::video
