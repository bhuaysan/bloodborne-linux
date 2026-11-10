// shadPS4's stb header. PNG writing (screenshots) is stb_image_write from third_party/stb. PNG
// decoding is used only for the overlay layers' icons and the splash image, which the port leaves
// out, so stb_image is not imported and its two functions end the process if they are reached.
#pragma once

#include <stb_image_write.h>

using stbi_uc = unsigned char;

stbi_uc* stbi_load_from_memory(const stbi_uc* buffer, int len, int* x, int* y,
                               int* channels_in_file, int desired_channels);
void stbi_image_free(void* retval_from_stbi_load);
