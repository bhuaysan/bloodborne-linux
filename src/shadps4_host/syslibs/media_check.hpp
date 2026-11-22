// What bb-run --check-system asks of FFmpeg (src/tools/system_check.hpp): the decoders and the
// demuxer that AvPlayer and Ajm open for the game's movies and sounds (docs/design/os-libraries.md).
// The portable package builds an FFmpeg with only these (cmake/portable.cmake); a system FFmpeg
// may lack one.
#pragma once

#include <string>
#include <vector>

namespace bb::shadps4 {

struct MediaSupport {
    std::vector<std::string> found;    // the names of the decoders and demuxers present
    std::vector<std::string> missing;  // and of those absent
};

// Looks up the H.264, AAC and MP3 decoders and the MP4 demuxer.
MediaSupport media_support();

}  // namespace bb::shadps4
