// The imported system libraries on the host layer (src/shadps4_host/syslibs,
// docs/design/os-libraries.md): their registration in the HLE registry, and the host pieces they
// run on (guest threads, paths, versions, the AAC decoder that is left out).

#include <unistd.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "core/file_sys/fs.h"
#include "core/libraries/ajm/ajm.h"
#include "core/libraries/ajm/ajm_aac.h"
#include "core/libraries/ajm/ajm_result.h"
#include "core/libraries/audio/audioout.h"
#include "core/libraries/avplayer/avplayer.h"
#include "core/libraries/ime/ime_dialog.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/process.h"
#include "core/libraries/kernel/threads.h"
#include "core/libraries/libs.h"
#include "core/libraries/pad/pad.h"
#include "core/libraries/system/commondialog.h"
#include "core/user_settings.h"
#include "hle/hle.hpp"
#include "hle/kernel/paths.hpp"
#include "hle/kernel/thread.hpp"
#include "hle/system/system.hpp"
#include "loader/dynamic.hpp"

namespace Libraries::AvPlayer {
// Defined in avplayer.cpp, not declared in its header.
AvPlayerHandle PS4_SYSV_ABI sceAvPlayerInit(AvPlayerInitData* data);
}  // namespace Libraries::AvPlayer

namespace {

// Registers the libraries once per process: the registry is global and a second call throws.
void init_once() {
    static std::once_flag once;
    std::call_once(once, [] {
        ::setenv("SDL_AUDIO_DRIVER", "dummy", 0);  // no sound device in tests
        Core::Loader::SymbolsResolver symbols;
        Libraries::InitHLELibs(&symbols);
    });
}

std::uintptr_t resolved(const char* library, const char* nid) {
    return bb::hle::resolve({nid, library, "libTestModule", bb::loader::stt_func});
}

template <class F>
std::uintptr_t address(F* f) {
    return reinterpret_cast<std::uintptr_t>(f);
}

TEST(SysLibsRegistration, FillsTheNidTable) {
    init_once();
    // Every LIB_FUNCTION line of the six libraries (Pad, AudioOut with its sub-libraries, Ajm,
    // AvPlayer, ImeDialog, CommonDialog) reached the registry: 255 lines, less AvPlayer's
    // sceAvPlayerPrintf, which upstream leaves commented out.
    EXPECT_EQ(Libraries::LinkedFunctionCount(), 254u);
    struct Row {
        const char *library, *nid;
        std::uintptr_t impl;
    };
    const Row rows[] = {
        {"libScePad", "6ncge5+l5Qs", address(&Libraries::Pad::scePadClose)},
        {"libSceAudioOut", "ekNvsT22rsY", address(&Libraries::AudioOut::sceAudioOutOpen)},
        {"libSceAjm", "AxoDrINp4J8", address(&Libraries::Ajm::sceAjmInstanceCreate)},
        {"libSceAvPlayer", "aS66RI0gGgo", address(&Libraries::AvPlayer::sceAvPlayerInit)},
        {"libSceImeDialog", "NUeBrN7hzf0", address(&Libraries::ImeDialog::sceImeDialogInit)},
        {"libSceCommonDialog", "uoUpLGNkygk",
         address(&Libraries::CommonDialog::sceCommonDialogInitialize)},
    };
    for (const Row& row : rows) {
        EXPECT_EQ(resolved(row.library, row.nid), row.impl) << row.library << " " << row.nid;
        EXPECT_EQ(bb::hle::binding(row.library, row.nid), bb::hle::Binding::implemented);
    }
    // AudioOut also registers under the names of its sub-libraries, as upstream does.
    EXPECT_NE(resolved("libSceAudioOutDeviceService", "cx2dYFbzIAg"), 0u);
    EXPECT_EQ(bb::hle::binding("libSceAudioOutDeviceService", "cx2dYFbzIAg"),
              bb::hle::Binding::implemented);
}

TEST(SysLibsRegistration, ANidRegisteredTwiceFails) {
    init_once();
    Core::Loader::SymbolsResolver symbols;
    // A NID of src/hle (libkernel's scePthreadCreate).
    EXPECT_THROW(LinkSymbolImpl(&symbols, "6UgtwV+0zb4", "libkernel", 1, "libkernel",
                                address(&Libraries::Pad::scePadClose),
                                Core::Loader::SymbolType::Function),
                 std::logic_error);
    // Every NID of the libraries is taken now, so a second registration fails at the first.
    EXPECT_THROW(Libraries::InitHLELibs(&symbols), std::logic_error);
}

TEST(SysLibsHost, ThreadRunsOnAGuestThreadAndStops) {
    std::atomic<bool> guest{false}, stopped{false};
    {
        Libraries::Kernel::Thread thread;
        EXPECT_FALSE(thread.Joinable());
        thread.Run([&](std::stop_token stop) {
            guest = !bb::hle::kernel::current_thread()->adopted;
            while (!stop.stop_requested()) {
                std::this_thread::yield();
            }
            stopped = true;
        });
        EXPECT_TRUE(thread.Joinable());
        thread.Stop();
        EXPECT_FALSE(thread.Joinable());
        EXPECT_TRUE(stopped);
        // Run again after Stop; the destructor stops it.
        stopped = false;
        thread.Run([&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                std::this_thread::yield();
            }
            stopped = true;
        });
    }
    EXPECT_TRUE(guest);
    EXPECT_TRUE(stopped);
}

TEST(SysLibsHost, GuestPathsResolveThroughTheKernel) {
    const std::filesystem::path game = std::filesystem::temp_directory_path() / "bb-syslibs-game";
    bb::hle::kernel::path_map().mount_game(game);
    Core::FileSys::MntPoints mounts;
    EXPECT_EQ(mounts.Open("/app0/movie/a.mp4"), nullptr);
    EXPECT_EQ(mounts.GetHostPath("/app0/movie/a.mp4"), game / "movie" / "a.mp4");
    EXPECT_EQ(mounts.GetHostPath("/nomount/a.mp4"), std::filesystem::path{});
}

TEST(SysLibsHost, VersionsAndUser) {
    s32 sdk = 0;
    EXPECT_EQ(Libraries::Kernel::sceKernelGetCompiledSdkVersion(&sdk), 0);
    EXPECT_EQ(sdk, 0x02000071);
    EXPECT_NE(Libraries::Kernel::sceKernelGetCompiledSdkVersion(nullptr), 0);

    Libraries::Kernel::SwVersionStruct sw{};
    EXPECT_EQ(Libraries::Kernel::sceKernelGetSystemSwVersion(&sw), 0);
    EXPECT_EQ(sw.hex_representation, 0x14008001u);
    EXPECT_STREQ(sw.text_representation, "14.008.001");

    User* const user = UserManagement.GetUserByPlayerIndex(1);
    ASSERT_NE(user, nullptr);
    EXPECT_EQ(user->user_id, bb::hle::system::user_id);
    EXPECT_EQ(UserManagement.GetUserByID(bb::hle::system::user_id), user);
    EXPECT_EQ(UserManagement.GetUserByPlayerIndex(2), nullptr);
    EXPECT_EQ(UserManagement.GetUserByID(-1), nullptr);
}

TEST(SysLibsHost, AacDecodeFailsWithUpstreamsCodecError) {
    using namespace Libraries::Ajm;
    AjmAacDecoder decoder(AjmFormatEncoding::S16, AjmAacCodecFlags{}, 2);
    std::array<u8, 64> in{};
    std::array<u8, 64> out{};
    std::span<u8> input(in);
    std::array<std::span<u8>, 1> chunks{std::span<u8>(out)};
    SparseOutputBuffer output(chunks);
    AjmInstanceGapless gapless{};
    const DecoderResult result = decoder.ProcessData(input, output, gapless);
    EXPECT_EQ(static_cast<u32>(result.result),
              static_cast<u32>(ORBIS_AJM_RESULT_CODEC_ERROR | ORBIS_AJM_RESULT_FATAL));
    EXPECT_EQ(result.samples_written, 0u);
    EXPECT_EQ(input.size(), in.size());  // nothing consumed
}

}  // namespace

// Its own main instead of gtest_main: it ends with _exit, as bb-run ends the game, also after
// --gtest_list_tests. At exit, ImeDialog's static UI (ime_dialog.cpp) would be destroyed after the
// ImGui core's static layer list (imgui_core.cpp), which its destructor still uses, in the order
// this binary's link puts them.
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    const int status = RUN_ALL_TESTS();
    std::fflush(nullptr);
    ::_exit(status);
}
