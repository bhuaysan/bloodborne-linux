// The process state that the runtime hands to libkernel before game code runs
// (docs/design/m3-core-os.md, "Process" and "libc runs the game's allocator").
#pragma once

#include <cstdint>

namespace bb::hle::kernel {

// Sets the address that libkernel 959qrazPIrg (name unknown) returns: the runtime address of the
// eboot's PT_SCE_PROCPARAM block. libc.prx's preinit reads the game's allocator tables from it.
// 0, the value before the first call, makes 959qrazPIrg return a null pointer.
void set_process_parameters(std::uintptr_t block);

}  // namespace bb::hle::kernel
