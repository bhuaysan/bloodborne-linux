// Binding the modules of a load set to each other and to the HLE layer (docs/design/m3-core-os.md,
// "Exports go through bb::hle").
#pragma once

#include <functional>

#include "loader/load_set.hpp"

namespace bb::hle {

// Registers every export of every module of `set` with register_export, those of the module named
// libc a second time under libSceLibcInternal unless a module exports the NID there itself. Then
// relocates each module with resolve() and its module id, so imports bind to the exports first.
// Last registers the modules' TLS templates with kernel::register_tls_modules.
// Throws as register_export, loader::relocate and register_tls_modules do; the images are then
// unusable.
void link(loader::LoadSet& set);

// Prepares a mapped load set to run: link(set), then `patch` on the eboot's image (bb-run applies
// the hooks there; empty for none), then finalize_protection on every module. Returns the eboot's
// image. Throws as link does, and std::logic_error for a set without an eboot.
loader::Image& prepare(loader::LoadSet& set,
                       const std::function<void(loader::Image& eboot)>& patch = {});

}  // namespace bb::hle
