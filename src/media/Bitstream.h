#pragma once

// Minimal bitstream inspection of compressed video packets, used to find
// the frames a clip may start from without re-encoding.

#include <cstddef>
#include <cstdint> // before FFmpeg headers: they need UINT64_C in C++

extern "C" {
#include <libavcodec/codec_id.h>
}

namespace adscutter {

struct FrameClass {
    bool known = false;        // codec is understood by the classifier
    bool randomAccess = false; // decoding can start here (IDR, IRAP, recovery point, MPEG-2 I)
    bool intra = false;        // first picture of the packet is intra coded
    bool paramSets = false;    // packet carries sequence/parameter sets
};

// Returns the NAL length field size for H.264/HEVC extradata in avcC/hvcC
// form (as in Matroska/MP4), or 0 for Annex B streams (as in MPEG-TS).
int nalLengthSize(AVCodecID codec, const uint8_t *extradata, int extradataSize);

FrameClass classifyVideoPacket(AVCodecID codec, const uint8_t *data, size_t size, int nalLengthSize);

} // namespace adscutter
