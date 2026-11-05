// Binding the modules of a load set to each other and to the HLE layer (docs/design/m3-core-os.md,
// "Exports go through bb::hle").
#pragma once

#include "loader/load_set.hpp"

namespace bb::hle {

// Registers every export of every module of `set` with register_export, those of the module named
// libc a second time under libSceLibcInternal unless a module exports the NID there itself. Then
// relocates each module with resolve() and its module id, so imports bind to the exports first.
// Throws as register_export and loader::relocate do; the images are then unusable.
void link(loader::LoadSet& set);

}  // namespace bb::hle
