#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace adscutter {

// Converts AAC in LATM/LOAS framing (as broadcast on DVB-T2) into raw AAC
// frames plus an AudioSpecificConfig, which is how Matroska stores AAC.
// The audio itself is copied unchanged; only the framing is removed.
//
// Supported: one program with one layer (what DVB uses), StreamMuxConfig
// versions 0 and 1, frameLengthType 0, any number of subframes.
class LatmConverter {
public:
    // Parses one demuxed packet (one or more LOAS frames). Appends the raw
    // AAC frames it carries to `frames`. Frames that precede the first
    // StreamMuxConfig cannot be decoded and are skipped.
    bool convert(const uint8_t *data, size_t size, std::vector<std::vector<uint8_t>> &frames, std::string *why);

    bool hasConfig() const { return !m_asc.empty(); }
    // Byte-aligned AudioSpecificConfig, for the container.
    const std::vector<uint8_t> &audioSpecificConfig() const { return m_asc; }
    // True once a StreamMuxConfig with a different AudioSpecificConfig than
    // the first one was seen (e.g. stereo -> 5.1 at a programme change).
    bool configChanged() const { return m_configChanged; }

private:
    class Bits;
    bool parseAudioMuxElement(Bits &b, std::vector<std::vector<uint8_t>> &frames, std::string *why);
    bool parseStreamMuxConfig(Bits &b, std::string *why);

    std::vector<uint8_t> m_asc;
    bool m_configChanged = false;
    int m_numSubFrames = 0;
    bool m_otherDataPresent = false;
    uint32_t m_otherDataLenBits = 0;
    bool m_crcPresent = false;
    int m_audioMuxVersion = 0;
};

} // namespace adscutter
