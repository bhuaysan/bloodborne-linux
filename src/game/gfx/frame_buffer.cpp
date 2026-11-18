// The engine's frame buffer size (docs/design/resolution.md, "Decision: way B").
#include "game/gfx/frame_buffer.hpp"

#include <sys/mman.h>

#include <cstring>

#include "common/memory.hpp"
#include "game/image.hpp"

namespace bb::game {

void set_frame_buffer_size(std::uint32_t width, std::uint32_t height) {
    global<std::uint32_t>(frame_buffer_width) = width;
    global<std::uint32_t>(frame_buffer_height) = height;
}

void set_graphics_heap_size(loader::Image& eboot, std::uint32_t width, std::uint32_t height) {
    const std::uint64_t size = graphics_private_heap_size(width, height);
    void* const at = reinterpret_cast<void*>(eboot.runtime(graphics_private_heap_size_entry));
    if (!eboot.finalized()) {
        std::memcpy(at, &size, sizeof size);
        return;
    }
    const loader::Segment* segment = eboot.segment_at(graphics_private_heap_size_entry, sizeof size);
    const std::uint64_t start = common::page_down(reinterpret_cast<std::uintptr_t>(at));
    const std::uint64_t end = common::page_up(reinterpret_cast<std::uintptr_t>(at) + sizeof size);
    void* const page = reinterpret_cast<void*>(start);
    common::protect(page, end - start, PROT_READ | PROT_WRITE, "set_graphics_heap_size: ");
    std::memcpy(at, &size, sizeof size);
    common::protect(page, end - start, segment->prot, "set_graphics_heap_size: ");
}

}  // namespace bb::game
