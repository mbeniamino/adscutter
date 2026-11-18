#pragma once

// Minimal bitstream inspection of compressed video packets, used to find
// the frames a clip may start from without re-encoding.

#include <cstddef>
#include <cstdint> // before FFmpeg headers: they need UINT64_C in C++
#include <vector>

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

// Returns the parameter sets carried by a packet (H.264 SPS/PPS, HEVC
// VPS/SPS/PPS, MPEG-1/2 sequence header with its extensions), framed like the
// packet itself (start codes or length prefixes), ready to be prepended to
// another packet. Empty if the packet has none.
std::vector<uint8_t> extractParamSets(AVCodecID codec, const uint8_t *data, size_t size, int nalLengthSize);

// HEVC: turns CRA pictures into BLA_W_LP in place, so that a decoder starting
// there (or continuing from a different segment) discards the leading
// pictures and restarts picture order counting. Returns true if changed.
bool hevcCraToBla(uint8_t *data, size_t size, int nalLengthSize);

} // namespace adscutter
