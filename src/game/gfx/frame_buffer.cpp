// The engine's frame buffer size (docs/design/resolution.md, "Decision: way B").
#include "game/gfx/frame_buffer.hpp"

#include <sys/mman.h>

#include <cstring>
#include <stdexcept>

#include "common/memory.hpp"
#include "game/image.hpp"

namespace bb::game {

void set_frame_buffer_size(std::uint32_t width, std::uint32_t height) {
    global<std::uint32_t>(frame_buffer_width) = width;
    global<std::uint32_t>(frame_buffer_height) = height;
}

void write_image_bytes(loader::Image& eboot, std::uint64_t link, const void* bytes,
                       std::size_t size) {
    void* const at = reinterpret_cast<void*>(eboot.runtime(link));
    if (!eboot.finalized()) {
        std::memcpy(at, bytes, size);
        return;
    }
    const loader::Segment* segment = eboot.segment_at(link, size);
    if (segment == nullptr) {
        throw std::out_of_range("write_image_bytes: the bytes lie outside a segment");
    }
    const std::uint64_t start = common::page_down(reinterpret_cast<std::uintptr_t>(at));
    const std::uint64_t end = common::page_up(reinterpret_cast<std::uintptr_t>(at) + size);
    void* const page = reinterpret_cast<void*>(start);
    common::protect(page, end - start, PROT_READ | PROT_WRITE, "write_image_bytes: ");
    std::memcpy(at, bytes, size);
    common::protect(page, end - start, segment->prot, "write_image_bytes: ");
}

void set_graphics_heap_size(loader::Image& eboot, std::uint32_t width, std::uint32_t height) {
    const std::uint64_t size = graphics_private_heap_size(width, height);
    write_image_bytes(eboot, graphics_private_heap_size_entry, &size, sizeof size);
}

}  // namespace bb::game
