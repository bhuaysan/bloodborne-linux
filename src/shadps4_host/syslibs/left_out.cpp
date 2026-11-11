// What the import leaves out of the system libraries (docs/design/os-libraries.md, "Codecs"):
// AJM's AAC decoder, which needs FDK-AAC, and AudioOut's OpenAL backend. Neither runs in the
// reference run; reaching one is logged.

#include <cstring>

#include "common/logging/log.h"
#include "core/libraries/ajm/ajm_aac.h"
#include "core/libraries/ajm/ajm_result.h"
#include "core/libraries/audio/audioout_backend.h"
#include "shadps4_host/failure.hpp"

namespace Libraries::Ajm {

// An AAC instance is created, so the game sees the instance it asked for, but every decode fails
// with what upstream's decoder reports when FDK-AAC fails: a fatal codec error.
AjmAacDecoder::AjmAacDecoder(AjmFormatEncoding format, AjmAacCodecFlags flags, u32 channels)
    : m_format(format), m_flags(flags), m_channels(channels) {
    LOG_ERROR(Lib_Ajm, "AAC instance: the AAC decoder is not built, every decode fails");
}

AjmAacDecoder::~AjmAacDecoder() = default;

void AjmAacDecoder::Reset() {}

void AjmAacDecoder::Initialize(const void*, u32) {}

void AjmAacDecoder::GetInfo(void* out_info) const {
    std::memset(out_info, 0, sizeof(AjmSidebandDecM4aacCodecInfo));
}

AjmSidebandFormat AjmAacDecoder::GetFormat() const {
    return {.num_channels = m_channels,
            .channel_mask = 0,
            .sampl_freq = 0,
            .sample_encoding = m_format,
            .bitrate = 0,
            .reserved = 0};
}

u32 AjmAacDecoder::GetMinimumInputSize() const {
    return 0;
}

u32 AjmAacDecoder::GetNextFrameSize(const AjmInstanceGapless&) const {
    return 0;
}

DecoderResult AjmAacDecoder::ProcessData(std::span<u8>&, SparseOutputBuffer&,
                                         AjmInstanceGapless&) {
    LOG_ERROR(Lib_Ajm, "AAC decode refused: the AAC decoder is not built");
    return {.result = static_cast<s32>(ORBIS_AJM_RESULT_CODEC_ERROR | ORBIS_AJM_RESULT_FATAL)};
}

}  // namespace Libraries::Ajm

namespace Libraries::AudioOut {

// EmulatorSettings fixes the SDL backend, so this is not reached.
std::unique_ptr<PortBackend> OpenALAudioOut::Open(PortOut&) {
    bb::shadps4::host_missing("AudioOut's OpenAL backend");
}

}  // namespace Libraries::AudioOut
