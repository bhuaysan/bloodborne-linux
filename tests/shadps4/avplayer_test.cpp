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
#include "core/libraries/avplayer/avplayer_common.h"
#include "core/libraries/avplayer/avplayer_source.h"
#include "core/libraries/libs.h"
#include "core/memory.h"
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
    if (avcodec_find_decoder(AV_CODEC_ID_AAC) == nullptr) {
        why = "no AAC decoder in this FFmpeg build; the player needs it for the movie's audio";
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

// The player's queues are shared between its demuxer, decoder and caller threads, which test their
// size while another thread pushes and pops (#539). The size, the check before a pop and the pop
// itself have to see the queue under its lock: an unguarded std::deque::size() can read the two
// ends mid-update, and libstdc++ treats the impossible size it then sees as unreachable.
TEST(AvPlayerQueue, SizeAndPopAreSafeAgainstConcurrentPushAndClear) {
    Libraries::AvPlayer::AvPlayerQueue<std::vector<int>> queue;
    constexpr std::size_t max_items = 8;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> bad_sizes{0}, bad_pops{0};
    // One producer keeps the queue between empty and max_items; the deque's ends move across its
    // blocks all the time, and its block map grows and shrinks.
    std::thread producer([&] {
        while (!stop) {
            if (queue.Size() < max_items) {
                queue.Push(std::vector<int>(1, 7));
            }
        }
    });
    // A consumer pops by its own check, as the decoder threads do, and a third thread empties
    // the queue, as Stop() does.
    std::thread consumer([&] {
        while (!stop) {
            if (queue.Size() != 0) {
                const auto item = queue.Pop();
                if (item.has_value() && (item->size() != 1 || item->front() != 7)) {
                    ++bad_pops;
                }
            }
            const auto popped = queue.PopIf([](const std::vector<int>& v) { return v.size() == 1; });
            if (popped.has_value() && popped->front() != 7) {
                ++bad_pops;
            }
        }
    });
    std::thread clearer([&] {
        while (!stop) {
            queue.Clear();
            std::this_thread::yield();
        }
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (queue.Size() > max_items) {
            ++bad_sizes;
        }
    }
    stop = true;
    producer.join();
    consumer.join();
    clearer.join();
    EXPECT_EQ(bad_sizes.load(), 0u);
    EXPECT_EQ(bad_pops.load(), 0u);
}

// The guest memory the library invalidates the GPU caches of the frames it writes in. It outlives
// the test, so a failed assertion that returns early can't leave the player's threads using a
// destroyed manager.
Core::MemoryManager& guest_memory() {
    static Core::MemoryManager memory;
    return memory;
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
        Common::Log::Level::Warning;  // Info is very noisy
    // The game opens its files under /app0/dvdroot_ps4, in the dump.
    bb::hle::kernel::path_map().mount_game(game);

    Core::Loader::SymbolsResolver symbols;
    RegisterLib(&symbols, guest_memory());

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

// The game's thread pulls frames while another thread stops the player (#785): Stop() resets the
// current frames and empties the buffer queues while a frame getter may be moving a current
// frame's buffer back into them. Each round starts the movie, lets the puller take a few frames
// (or none, to stop during start-up) and stops it under the puller's feet.
class NoSyncCallback final : public AvPlayerStateCallback {
public:
    AvPlayerAvSyncMode GetSyncMode() override {
        return AvPlayerAvSyncMode::None;  // frames as fast as they decode
    }
    void OnWarning(u32) override {}
    void OnError() override {}
    void OnEOF() override {}
};

// The player's buffers that are allocated and not yet freed: after Stop() there are none, since it
// frees the current frames and empties every queue. A getter that raced Stop() would keep one.
std::atomic<int> live_buffers{0};
void* PS4_SYSV_ABI allocate_counted(void* object, u32 align, u32 size) {
    void* p = allocate(object, align, size);
    if (p != nullptr) {
        ++live_buffers;
    }
    return p;
}
void PS4_SYSV_ABI deallocate_counted(void* object, void* mem) {
    if (mem != nullptr) {
        --live_buffers;
    }
    deallocate(object, mem);
}

TEST(AvPlayerGame, StopIsSafeAgainstAThreadPullingFrames) {
    BB_REQUIRE_GAME(game);
    const std::filesystem::path movie = game / "dvdroot_ps4/movie/sprj_opening.mp4";
    BB_REQUIRE_GAME_DATA(movie, "nothing: the game dump lacks its opening movie");
    if (avcodec_find_decoder(AV_CODEC_ID_H264) == nullptr ||
        avcodec_find_decoder(AV_CODEC_ID_AAC) == nullptr) {
        GTEST_SKIP() << "no H.264 or AAC decoder in this FFmpeg build";
    }
    Common::Log::g_class_levels[static_cast<std::size_t>(Common::Log::Class::Lib_AvPlayer)] =
        Common::Log::Level::Error;  // each round logs its start and stop
    bb::hle::kernel::path_map().mount_game(game);

    NoSyncCallback callback;
    AvPlayerInitData init{};
    init.memory_replacement = {nullptr, allocate_counted, deallocate_counted, allocate_counted,
                               deallocate_counted};
    init.num_output_video_framebuffers = 2;
    AvPlayerSource source(callback, guest_memory());
    ASSERT_TRUE(source.Init(init, "/app0/dvdroot_ps4/movie/sprj_opening.mp4"));
    ASSERT_TRUE(source.FindStreams());
    const s32 streams = source.GetStreamCount();
    ASSERT_GE(streams, 2);
    for (s32 i = 0; i < streams; ++i) {
        ASSERT_TRUE(source.EnableStream(i));
    }

    // The puller runs through every round, so it is inside a getter whenever Stop() comes.
    std::atomic<bool> done{false};
    std::atomic<int> video_frames{0}, bad_frames{0};
    std::thread puller([&] {
        while (!done) {
            AvPlayerFrameInfoEx video{};
            if (source.GetVideoData(video)) {
                if (video.p_data == nullptr) {
                    ++bad_frames;
                }
                ++video_frames;
            }
            AvPlayerFrameInfo audio{};
            if (source.GetAudioData(audio) && audio.p_data == nullptr) {
                ++bad_frames;
            }
        }
    });
    // A failed ASSERT returns early: stop and join the puller, or ~thread would abort the binary.
    struct JoinPuller {
        std::atomic<bool>& done;
        std::thread& thread;
        ~JoinPuller() {
            done = true;
            thread.join();
        }
    } join_puller{done, puller};

    constexpr int rounds = 500;
    int rounds_with_frames = 0;
    for (int round = 0; round < rounds; ++round) {
        ASSERT_TRUE(source.Start()) << "round " << round;
        // Every other round stops at once, the others after 1 to 3 frames.
        const int want = round % 2 == 0 ? 0 : 1 + round % 3;
        const int before = video_frames.load();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (video_frames.load() - before < want && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        if (video_frames.load() - before >= want && want > 0) {
            ++rounds_with_frames;
        }
        ASSERT_TRUE(source.Stop()) << "round " << round;
        ASSERT_EQ(live_buffers.load(), 0) << "round " << round;
    }
    EXPECT_EQ(bad_frames.load(), 0);
    // Playback restarts after each Stop(): every round that waited for frames got them.
    EXPECT_EQ(rounds_with_frames, rounds / 2);
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
