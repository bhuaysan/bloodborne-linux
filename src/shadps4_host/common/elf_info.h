// What src/shadps4 reads of shadPS4's information about the running game: its serial, for the
// names of screenshots and cache files, the splash image, which the port doesn't show, and the
// firmware version it needs, which selects Pad's touch data layout.
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

    // Upstream's FW_350: firmware 3.50.
    static constexpr u32 FW_350 = 0x3500000;

    // The firmware the game needs: SYSTEM_VER of its sce_sys/param.sfo (0x04500000, 4.50), with
    // the low 20 bits cleared as upstream does.
    u32 FirmwareVer() const { return 0x04500000; }

    // Empty: no splash.
    const std::vector<u8>& GetSplashData() const { return splash_data; }

private:
    std::vector<u8> splash_data;
};

}  // namespace Common
