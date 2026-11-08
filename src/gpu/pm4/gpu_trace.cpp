#include "gpu/pm4/gpu_trace.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <format>

#include "si_ci_vi_merged_offset.h"

namespace bb::pm4 {

namespace {

namespace amd = Pal::Gfx6::Chip;

constexpr std::size_t shown_dwords = 4;

bool is_draw(std::uint8_t opcode) {
    switch (opcode) {
    case IT_DRAW_INDIRECT:
    case IT_DRAW_INDEX_INDIRECT:
    case IT_DRAW_INDEX_2:
    case IT_DRAW_INDIRECT_MULTI:
    case IT_DRAW_INDEX_AUTO:
    case IT_DRAW_INDEX_MULTI_AUTO:
    case IT_DRAW_INDEX_OFFSET_2:
    case IT_DRAW_INDEX_INDIRECT_MULTI:
        return true;
    default:
        return false;
    }
}

bool is_dispatch(std::uint8_t opcode) {
    return opcode == IT_DISPATCH_DIRECT || opcode == IT_DISPATCH_INDIRECT;
}

std::string body_text(const Packet& packet) {
    std::string out;
    const std::size_t n = std::min(packet.body.size(), shown_dwords);
    for (std::size_t i = 0; i < n; ++i) {
        out += std::format(" {:#x}", packet.body[i]);
    }
    return out;
}

}  // namespace

GpuTrace::~GpuTrace() {
    if (sink_ != nullptr) {
        std::fclose(sink_);
    }
}

std::unique_ptr<GpuTrace> GpuTrace::open(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        return nullptr;
    }
    std::FILE* const file = ::fdopen(fd, "w");
    if (file == nullptr) {
        ::close(fd);
        return nullptr;
    }
    return std::make_unique<GpuTrace>(file);
}

std::string GpuTrace::describe(const Packet& packet, const RegisterMirror& regs) {
    const std::uint8_t opcode = packet.opcode();
    if (is_draw(opcode)) {
        return std::format(
            "draw op={:#04x} body={}{} vs={:#x} ps={:#x} rt0={:#x} prim={:#x}", opcode,
            packet.body.size(), body_text(packet), regs.program_address(Stage::vs),
            regs.program_address(Stage::ps), std::uint64_t{regs.get(amd::mmCB_COLOR0_BASE)} << 8,
            regs.get(amd::mmVGT_PRIMITIVE_TYPE__CI__VI));
    }
    if (is_dispatch(opcode)) {
        return std::format("dispatch op={:#04x} body={}{} cs={:#x} threads={}x{}x{}", opcode,
                           packet.body.size(), body_text(packet),
                           regs.program_address(Stage::cs), regs.num_threads(0),
                           regs.num_threads(1), regs.num_threads(2));
    }
    return {};
}

void GpuTrace::packet(const Packet& packet, const RegisterMirror& regs) {
    ++opcodes_[packet.opcode()];
    const std::string line = describe(packet, regs);
    if (line.empty()) {
        return;
    }
    (is_draw(packet.opcode()) ? draws_ : dispatches_) += 1;
    const std::string source = ring_ < 0 ? std::string() : std::format(" ring{}", ring_);
    std::fprintf(sink_, "frame %llu%s: %s\n", static_cast<unsigned long long>(frame_ + 1),
                 source.c_str(), line.c_str());
}

void GpuTrace::end_frame() {
    ++frame_;
    std::uint64_t total = 0;
    std::string ops;
    for (std::size_t op = 0; op < opcodes_.size(); ++op) {
        if (opcodes_[op] != 0) {
            total += opcodes_[op];
            ops += std::format(" {:#04x}x{}", op, opcodes_[op]);
        }
    }
    std::fprintf(sink_, "frame %llu end: %llu packets, %llu draws, %llu dispatches; opcodes:%s\n",
                 static_cast<unsigned long long>(frame_), static_cast<unsigned long long>(total),
                 static_cast<unsigned long long>(draws_),
                 static_cast<unsigned long long>(dispatches_), ops.c_str());
    std::fflush(sink_);
    opcodes_.fill(0);
    draws_ = 0;
    dispatches_ = 0;
}

}  // namespace bb::pm4
