#include "media/Latm.h"

namespace adscutter {

class LatmConverter::Bits {
public:
    Bits(const uint8_t *data, size_t size)
        : m_data(data)
        , m_bits(size * 8)
    {
    }
    size_t pos() const { return m_pos; }
    void seek(size_t pos) { m_pos = pos; }
    bool overrun() const { return m_pos > m_bits; }
    size_t left() const { return m_pos < m_bits ? m_bits - m_pos : 0; }
    uint32_t bit()
    {
        const uint32_t b = m_pos < m_bits ? (m_data[m_pos >> 3] >> (7 - (m_pos & 7))) & 1 : 0;
        ++m_pos;
        return b;
    }
    uint32_t bits(int n)
    {
        uint32_t v = 0;
        while (n-- > 0)
            v = (v << 1) | bit();
        return v;
    }
    // LatmGetValue()
    uint32_t latmValue()
    {
        const int bytes = int(bits(2));
        uint32_t v = 0;
        for (int i = 0; i <= bytes; ++i)
            v = (v << 8) | bits(8);
        return v;
    }
    // Copies bits [from, to) into bytes, zero padded.
    std::vector<uint8_t> extract(size_t from, size_t to) const
    {
        std::vector<uint8_t> out((to - from + 7) / 8, 0);
        for (size_t p = from; p < to; ++p) {
            if (p < m_bits && ((m_data[p >> 3] >> (7 - (p & 7))) & 1))
                out[(p - from) >> 3] |= uint8_t(0x80 >> ((p - from) & 7));
        }
        return out;
    }

private:
    const uint8_t *m_data;
    size_t m_bits;
    size_t m_pos = 0;
};

namespace {

// Reads an AudioSpecificConfig (ISO 14496-3 1.6.2.1) just far enough to know
// where it ends.
template <typename B>
bool skipAudioSpecificConfig(B &b, std::string *why)
{
    auto objectType = [&] {
        uint32_t t = b.bits(5);
        return t == 31 ? 32 + b.bits(6) : t;
    };
    auto frequency = [&] {
        if (b.bits(4) == 15)
            b.bits(24);
    };
    uint32_t aot = objectType();
    frequency();
    const uint32_t channels = b.bits(4);
    if (aot == 5 || aot == 29) { // explicit SBR / PS
        frequency();
        aot = objectType();
    }
    switch (aot) {
    case 1: case 2: case 3: case 4: case 6: case 7:
    case 17: case 19: case 20: case 21: case 22: case 23: {
        b.bit(); // frameLengthFlag
        if (b.bit())
            b.bits(14); // coreCoderDelay
        const bool extension = b.bit();
        if (channels == 0) {
            if (why)
                *why = "AAC with a program config element is not supported";
            return false;
        }
        if (aot == 6 || aot == 20)
            b.bits(3);
        if (extension) {
            if (aot == 22)
                b.bits(16);
            if (aot == 17 || aot == 19 || aot == 20 || aot == 23)
                b.bits(3);
            b.bit(); // extensionFlag3
        }
        break;
    }
    default:
        if (why)
            *why = "unsupported AAC object type " + std::to_string(aot);
        return false;
    }
    if (aot >= 17 && aot <= 27 && aot != 18 && b.bits(2) >= 2) {
        if (why)
            *why = "AAC error protection is not supported";
        return false;
    }
    return !b.overrun();
}

} // namespace

bool LatmConverter::parseStreamMuxConfig(Bits &b, std::string *why)
{
    auto fail = [&](const char *reason) {
        if (why)
            *why = reason;
        return false;
    };
    m_audioMuxVersion = int(b.bit());
    if (m_audioMuxVersion && b.bit())
        return fail("LATM audioMuxVersionA 1 is not supported");
    if (m_audioMuxVersion)
        b.latmValue(); // taraBufferFullness
    if (!b.bit())
        return fail("LATM streams with separate time framing are not supported");
    m_numSubFrames = int(b.bits(6));
    if (b.bits(4) != 0)
        return fail("LATM with several programs is not supported");
    if (b.bits(3) != 0)
        return fail("LATM with several layers is not supported");

    size_t ascStart = b.pos(), ascEnd;
    if (m_audioMuxVersion == 0) {
        if (!skipAudioSpecificConfig(b, why))
            return false;
        ascEnd = b.pos();
    } else {
        const uint32_t ascLen = b.latmValue();
        ascStart = b.pos();
        Bits probe = b;
        if (!skipAudioSpecificConfig(probe, why))
            return false;
        ascEnd = probe.pos();
        b.seek(ascStart + ascLen);
    }
    if (b.bits(3) != 0)
        return fail("LATM frameLengthType other than 0 is not supported");
    b.bits(8); // latmBufferFullness
    m_otherDataPresent = b.bit();
    m_otherDataLenBits = 0;
    if (m_otherDataPresent) {
        if (m_audioMuxVersion) {
            m_otherDataLenBits = b.latmValue();
        } else {
            bool escape;
            do {
                escape = b.bit();
                m_otherDataLenBits = (m_otherDataLenBits << 8) + b.bits(8);
            } while (escape && !b.overrun());
        }
    }
    m_crcPresent = b.bit();
    if (m_crcPresent)
        b.bits(8);
    if (b.overrun())
        return fail("truncated LATM configuration");

    std::vector<uint8_t> asc = b.extract(ascStart, ascEnd);
    if (m_asc.empty())
        m_asc = std::move(asc);
    else if (asc != m_asc)
        m_configChanged = true;
    return true;
}

bool LatmConverter::parseAudioMuxElement(Bits &b, std::vector<std::vector<uint8_t>> &frames, std::string *why)
{
    if (!b.bit()) { // !useSameStreamMux
        if (!parseStreamMuxConfig(b, why))
            return false;
    }
    if (m_asc.empty())
        return true; // nothing decodable before the first configuration

    for (int i = 0; i <= m_numSubFrames; ++i) {
        // PayloadLengthInfo (frameLengthType 0): bytes, 255-escaped.
        size_t length = 0;
        uint32_t tmp;
        do {
            tmp = b.bits(8);
            length += tmp;
        } while (tmp == 255 && !b.overrun());
        const size_t start = b.pos();
        if (b.left() < length * 8) {
            if (why)
                *why = "truncated LATM payload";
            return false;
        }
        frames.push_back(b.extract(start, start + length * 8));
        b.seek(start + length * 8);
    }
    return true;
}

bool LatmConverter::convert(const uint8_t *data, size_t size, std::vector<std::vector<uint8_t>> &frames,
                            std::string *why)
{
    // AudioSyncStream: syncword 0x2B7 (11 bits), audioMuxLengthBytes (13 bits).
    size_t pos = 0;
    while (pos + 3 <= size) {
        if (data[pos] != 0x56 || (data[pos + 1] & 0xe0) != 0xe0) {
            if (why)
                *why = "LOAS sync word not found";
            return false;
        }
        const size_t length = size_t((data[pos + 1] & 0x1f) << 8 | data[pos + 2]);
        if (pos + 3 + length > size) {
            if (why)
                *why = "truncated LOAS frame";
            return false;
        }
        Bits b(data + pos + 3, length);
        if (!parseAudioMuxElement(b, frames, why))
            return false;
        pos += 3 + length;
    }
    return true;
}

} // namespace adscutter
