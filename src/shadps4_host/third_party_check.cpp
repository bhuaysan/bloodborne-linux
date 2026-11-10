// Compiles the include surface of the libraries video_core needs (issue #427), so that a broken
// copy or include path fails the build before anything uses them.
#include <imgui.h>
#include <stb_image_write.h>
#include <tsl/robin_map.h>
#include <vk_mem_alloc.h>
#include <xxhash.h>

namespace bb {

unsigned long long third_party_check() {
    tsl::robin_map<int, int> map;
    map[1] = 2;
    return XXH3_64bits("a", 1) + static_cast<unsigned long long>(map.size()) +
           static_cast<unsigned long long>(IMGUI_VERSION_NUM) + sizeof(VmaAllocatorCreateInfo) +
           reinterpret_cast<unsigned long long>(&stbi_write_png_to_func);
}

} // namespace bb
