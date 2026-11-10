// What video_core reads of shadPS4's information about the running game: its serial, for the
// names of screenshots and cache files, and the splash image, which the port doesn't show.
#pragma once

#include <string_view>
#include <vector>

#include "common/singleton.h"
#include "common/types.h"

namespace Common {

class ElfInfo {
public:
    static ElfInfo& Instance() { return *Singleton<ElfInfo>::Instance(); }

    // The one game this port runs (docs/source-policy.md, "Game version").
    std::string_view GameSerial() const { return "CUSA03173"; }

    // Empty: no splash.
    const std::vector<u8>& GetSplashData() const { return splash_data; }

private:
    std::vector<u8> splash_data;
};

}  // namespace Common
