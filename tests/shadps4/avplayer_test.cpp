// The imported AvPlayer on the host layer (docs/design/os-libraries.md): it opens the game's
// opening movie through our guest paths, and its first video frames match what FFmpeg decodes
// from the same file. The game test needs the dump and an H.264 decoder: Fedora's ffmpeg-free
// (the CI build) has none, RPM Fusion's FFmpeg does.

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#include "common/logging/log.h"
#include "core/libraries/avplayer/avplayer.h"
#include "hle/kernel/paths.hpp"
#include "hle/kernel/thread.hpp"
#include "support/game.hpp"

namespace Libraries::AvPlayer {
// Defined in avplayer.cpp, not declared in its header.
AvPlayerHandle PS4_SYSV_ABI sceAvPlayerInit(AvPlayerInitData* data);
s32 PS4_SYSV_ABI sceAvPlayerAddSource(AvPlayerHandle handle, const char* filename);
s32 PS4_SYSV_ABI sceAvPlayerClose(AvPlayerHandle handle);
bool PS4_SYSV_ABI sceAvPlayerGetAudioData(AvPlayerHandle handle, AvPlayerFrameInfo* p_info);
bool PS4_SYSV_ABI sceAvPlayerGetVideoDataEx(AvPlayerHandle handle,
                                            AvPlayerFrameInfoEx* video_info);
s32 PS4_SYSV_ABI sceAvPlayerStop(AvPlayerHandle handle);
s32 PS4_SYSV_ABI sceAvPlayerStreamCount(AvPlayerHandle handle);
}  // namespace Libraries::AvPlayer

namespace {

using namespace Libraries::AvPlayer;

constexpr int frames_to_compare = 30;

// What the game's allocators do: align, size, and a texture allocator for the video frames.
// Counts the calls and which of them came from a thread that is not a guest thread.
std::atomic<int> allocations{0}, host_thread_calls{0};

void note_thread() {
    ++allocations;
    if (bb::hle::kernel::current_thread()->adopted) {
        ++host_thread_calls;
    }
}

void* PS4_SYSV_ABI allocate(void*, u32 align, u32 size) {
    note_thread();
    void* p = nullptr;
    return ::posix_memalign(&p, std::max<std::size_t>(align, sizeof(void*)), size) == 0 ? p
                                                                                         : nullptr;
}
void PS4_SYSV_ABI deallocate(void*, void* mem) {
    std::free(mem);
}

std::atomic<int> state_events{0};
void PS4_SYSV_ABI on_event(void*, AvPlayerEvents, s32, void*) {
    ++state_events;
}

struct Nv12Frame {
    std::vector<std::uint8_t> luma, chroma;  // width * height, and width * height / 2
};

// FFmpeg's own decode of the first video frames, converted to NV12 as the player does.
// Returns false (with a reason) when the file or an H.264 decoder is not usable.
bool reference_frames(const std::string& path, std::vector<Nv12Frame>& out, int& width, int& height,
                      std::string& why) {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (codec == nullptr) {
        why = "no H.264 decoder in this FFmpeg build (ffmpeg-free; RPM Fusion's FFmpeg has one)";
        return false;
    }
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0 ||
        avformat_find_stream_info(fmt, nullptr) < 0) {
        why = "FFmpeg cannot open " + path;
        return false;
    }
    const int stream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    SwsContext* sws = nullptr;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool ok = stream >= 0 && avcodec_parameters_to_context(ctx, fmt->streams[stream]->codecpar) >= 0 &&
              avcodec_open2(ctx, codec, nullptr) >= 0;
    if (!ok) {
        why = "FFmpeg cannot open the H.264 decoder";
    }
    while (ok && static_cast<int>(out.size()) < frames_to_compare &&
           av_read_frame(fmt, packet) >= 0) {
        if (packet->stream_index == stream && avcodec_send_packet(ctx, packet) >= 0) {
            while (static_cast<int>(out.size()) < frames_to_compare &&
                   avcodec_receive_frame(ctx, frame) >= 0) {
                width = frame->width;
                height = frame->height;
                if (sws == nullptr) {
                    sws = sws_getContext(width, height, AVPixelFormat(frame->format), width,
                                         height, AV_PIX_FMT_NV12, SWS_FAST_BILINEAR, nullptr,
                                         nullptr, nullptr);
                }
                Nv12Frame nv;
                nv.luma.resize(std::size_t(width) * height);
                nv.chroma.resize(std::size_t(width) * height / 2);
                std::uint8_t* dst[4] = {nv.luma.data(), nv.chroma.data(), nullptr, nullptr};
                const int dst_stride[4] = {width, width, 0, 0};
                sws_scale(sws, frame->data, frame->linesize, 0, height, dst, dst_stride);
                out.push_back(std::move(nv));
            }
        }
        av_packet_unref(packet);
    }
    ok = ok && static_cast<int>(out.size()) == frames_to_compare;
    if (!ok && why.empty()) {
        why = "FFmpeg decoded fewer than " + std::to_string(frames_to_compare) + " frames";
    }
    sws_freeContext(sws);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    return ok;
}

TEST(AvPlayerGame, OpeningMovieFramesMatchFfmpeg) {
    BB_REQUIRE_GAME(game);
    const std::filesystem::path movie = game / "dvdroot_ps4/movie/sprj_opening.mp4";
    BB_REQUIRE_GAME_DATA(movie, "nothing: the game dump lacks its opening movie");

    std::vector<Nv12Frame> reference;
    int width = 0, height = 0;
    std::string why;
    if (!reference_frames(movie.string(), reference, width, height, why)) {
        GTEST_SKIP() << why;
    }
    EXPECT_EQ(width, 1920);
    EXPECT_EQ(height, 1080);

    Common::Log::g_class_levels[static_cast<std::size_t>(Common::Log::Class::Lib_AvPlayer)] =
        Common::Log::Level::Info;
    // The game opens its files under /app0/dvdroot_ps4, in the dump.
    bb::hle::kernel::path_map().mount_game(game);

    AvPlayerInitData init{};
    init.memory_replacement = {nullptr, allocate, deallocate, allocate, deallocate};
    init.event_replacement = {nullptr, on_event};
    init.num_output_video_framebuffers = 4;
    init.auto_start = true;
    init.default_language = "eng";
    AvPlayerHandle player = sceAvPlayerInit(&init);
    ASSERT_NE(player, nullptr);
    ASSERT_EQ(sceAvPlayerAddSource(player, "/app0/dvdroot_ps4/movie/sprj_opening.mp4"), 0);

    int streams = 0;
    for (int i = 0; i < 100 && (streams = sceAvPlayerStreamCount(player)) <= 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_GE(streams, 2);  // video and audio

    // The frames come at the movie's pace (30 fps): a second, and room for a slow machine.
    const std::size_t pitch = (width + 63) / 64 * 64;
    const std::size_t padded_height = (height + 15) / 16 * 16;
    int got = 0, audio_frames = 0;
    std::uint64_t last_time = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (got < frames_to_compare && std::chrono::steady_clock::now() < deadline) {
        AvPlayerFrameInfo audio{};
        if (sceAvPlayerGetAudioData(player, &audio)) {
            ++audio_frames;
            EXPECT_GE(audio.details.audio.channel_count, 1);
        }
        AvPlayerFrameInfoEx info{};
        if (!sceAvPlayerGetVideoDataEx(player, &info)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        ASSERT_NE(info.p_data, nullptr);
        EXPECT_GE(info.timestamp, last_time);
        last_time = info.timestamp;
        EXPECT_EQ(info.details.video.width, 1920u);
        EXPECT_EQ(info.details.video.height, padded_height);
        EXPECT_EQ(info.details.video.pitch, pitch);
        // Same decoder and same conversion: identical, row by row (the player's rows are pitched).
        const auto* luma = static_cast<const std::uint8_t*>(info.p_data);
        const auto* chroma = luma + pitch * padded_height;
        const Nv12Frame& ref = reference[got];
        bool same = true;
        for (int y = 0; y < height && same; ++y) {
            same = std::memcmp(luma + y * pitch, ref.luma.data() + std::size_t(y) * width, width) == 0;
        }
        for (int y = 0; y < height / 2 && same; ++y) {
            same = std::memcmp(chroma + y * pitch, ref.chroma.data() + std::size_t(y) * width,
                               width) == 0;
        }
        EXPECT_TRUE(same) << "frame " << got << " differs from FFmpeg's decode";
        ++got;
    }
    EXPECT_EQ(got, frames_to_compare);
    // 30 frames of a 30 fps movie span 29 frame times.
    EXPECT_NEAR(static_cast<double>(last_time), 966.0, 40.0);
    EXPECT_GT(audio_frames, 0);
    EXPECT_GT(state_events.load(), 0);
    EXPECT_GT(allocations.load(), 0);
    // The player's threads are guest threads, so the game's allocators run on them.
    EXPECT_EQ(host_thread_calls.load(), 0);

    EXPECT_EQ(sceAvPlayerStop(player), 0);
    EXPECT_EQ(sceAvPlayerClose(player), 0);
}

}  // namespace

// Its own main, ending with _exit like shadps4_syslibs_test: the libraries' static UI would
// otherwise be destroyed after the ImGui core's static layer list.
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    const int status = RUN_ALL_TESTS();
    std::fflush(nullptr);
    ::_exit(status);
}
