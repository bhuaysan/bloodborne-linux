// The GPU trace (docs/design/graphics.md, "GPU trace"): per frame, the packets by opcode and one
// line per draw and dispatch with the registers that decide what it runs. A frame ends when the
// GPU thread has finished the last draw command buffer of a submit-and-flip. The trace is written
// by the GPU thread only; it is off unless the GpuThread is given one.
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "gpu/pm4/packet.hpp"
#include "gpu/pm4/registers.hpp"

namespace bb::pm4 {

class GpuTrace {
public:
    // Takes `sink`, closed by the destructor.
    explicit GpuTrace(std::FILE* sink) : sink_(sink) {}
    ~GpuTrace();
    GpuTrace(const GpuTrace&) = delete;
    GpuTrace& operator=(const GpuTrace&) = delete;

    // Creates `path` (mode 0600, not overwriting) for writing; null with errno set on failure.
    static std::unique_ptr<GpuTrace> open(const std::filesystem::path& path);

    // A packet that ran, with the register mirror as it is after the packet.
    void packet(const Packet& packet, const RegisterMirror& regs);
    // The stream the next packets come from: a compute ring index, or -1 for a submitted one.
    // Lines of a ring stream read "frame N ringK:".
    void set_source(int ring) { ring_ = ring; }
    // Closes the frame: its packet counts by opcode, then flushes the file.
    void end_frame();

    std::uint64_t frames() const { return frame_; }

    // The trace line of a draw or dispatch packet, or empty for any other packet.
    static std::string describe(const Packet& packet, const RegisterMirror& regs);

private:
    std::FILE* sink_;
    std::uint64_t frame_ = 0;
    int ring_ = -1;
    std::uint64_t draws_ = 0;
    std::uint64_t dispatches_ = 0;
    std::array<std::uint32_t, 256> opcodes_{};
};

}  // namespace bb::pm4
