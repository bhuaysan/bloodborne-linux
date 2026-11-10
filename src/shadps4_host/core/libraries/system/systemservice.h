// The piece of shadPS4's system service header that the presenter uses.
#pragma once

namespace Libraries::SystemService {

// No splash screen: the port shows none (ElfInfo has no splash image either).
constexpr bool IsSplashVisible() { return false; }

}  // namespace Libraries::SystemService
