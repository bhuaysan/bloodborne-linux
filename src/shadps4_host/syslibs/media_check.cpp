#include "syslibs/media_check.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace bb::shadps4 {

MediaSupport media_support() {
    MediaSupport out;
    // AvPlayer: the game's movies are H.264 with AAC sound in MP4 (AvPlayer also takes HEVC,
    // which no movie of the game uses and Fedora's ffmpeg-free lacks); Ajm: MP3.
    for (const auto& [id, name] : {std::pair{AV_CODEC_ID_H264, "h264"},
                                   std::pair{AV_CODEC_ID_AAC, "aac"},
                                   std::pair{AV_CODEC_ID_MP3, "mp3"}}) {
        (avcodec_find_decoder(id) != nullptr ? out.found : out.missing).emplace_back(name);
    }
    // One demuxer has the names mov, mp4, m4a and others; any of them finds it.
    (av_find_input_format("mp4") != nullptr ? out.found : out.missing).emplace_back("mp4 demuxer");
    return out;
}

}  // namespace bb::shadps4
