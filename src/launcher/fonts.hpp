#pragma once

namespace bb::launcher {

// Adds a serif font of the system (found with fontconfig: EB Garamond, Cormorant Garamond, else
// the "serif" match) at `size` ImGui units to the current ImGui context, and ImGui's own font
// when there is none. Returns true for the system font. Nothing is shipped with the launcher.
bool load_fonts(float size);

}  // namespace bb::launcher
