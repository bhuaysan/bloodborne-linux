#pragma once

namespace bb::launcher {

// Adds the system's sans-serif font (found with fontconfig) at `size` ImGui units to the current
// ImGui context, and ImGui's own font when there is none. Returns true for the system font.
bool load_fonts(float size);

}  // namespace bb::launcher
