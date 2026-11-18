#include "media/Bitstream.h"

#include <array>

namespace adscutter {

namespace {

// Reads bits from an RBSP (emulation prevention bytes already removed).
class BitReader {
public:
    BitReader(const uint8_t *data, size_t size)
        : m_data(data)
        , m_bits(size * 8)
    {
    }

    bool overrun() const { return m_pos > m_bits; }

    uint32_t bits(int n)
    {
        uint32_t v = 0;
        while (n-- > 0) {
            v <<= 1;
            if (m_pos < m_bits)
                v |= (m_data[m_pos >> 3] >> (7 - (m_pos & 7))) & 1;
            ++m_pos;
        }
        return v;
    }

    // Unsigned Exp-Golomb.
    uint32_t ue()
    {
        int zeros = 0;
        while (bits(1) == 0) {
            if (++zeros > 31 || overrun()) {
                m_pos = m_bits + 1;
                return 0;
            }
        }
        return ((1u << zeros) - 1) + bits(zeros);
    }

private:
    const uint8_t *m_data;
    size_t m_bits;
    size_t m_pos = 0;
};

// Copies at most out.size() bytes of RBSP from a NAL payload, dropping
// emulation prevention bytes (00 00 03 -> 00 00).
template <size_t N>
size_t unescape(const uint8_t *src, size_t size, std::array<uint8_t, N> &out)
{
    size_t o = 0;
    int zeros = 0;
    for (size_t i = 0; i < size && o < N; ++i) {
        if (zeros >= 2 && src[i] == 3) {
            zeros = 0;
            continue;
        }
        zeros = src[i] == 0 ? zeros + 1 : 0;
        out[o++] = src[i];
    }
    return o;
}

// Calls f(nal, size) for every NAL unit of a packet.
template <typename F>
void forEachNal(const uint8_t *data, size_t size, int lengthSize, F &&f)
{
    if (lengthSize > 0) {
        size_t pos = 0;
        while (pos + size_t(lengthSize) <= size) {
            size_t len = 0;
            for (int i = 0; i < lengthSize; ++i)
                len = (len << 8) | data[pos + i];
            pos += lengthSize;
            if (len == 0 || len > size - pos)
                break;
            if (!f(data + pos, len))
                return;
            pos += len;
        }
        return;
    }

    // Annex B: NAL units separated by 00 00 01 (or 00 00 00 01).
    auto findStart = [&](size_t from) -> size_t {
        for (size_t i = from; i + 3 <= size; ++i) {
            if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
                return i;
        }
        return size;
    };
    size_t start = findStart(0);
    while (start < size) {
        const size_t nal = start + 3;
        size_t next = findStart(nal);
        size_t end = next;
        while (end > nal && data[end - 1] == 0) // trailing zero / 4-byte start code
            --end;
        if (end > nal && !f(data + nal, end - nal))
            return;
        start = next;
    }
}

FrameClass classifyH264(const uint8_t *data, size_t size, int lengthSize)
{
    FrameClass c;
    c.known = true;
    bool idr = false, recovery = false, sps = false, pps = false, sliceSeen = false;

    forEachNal(data, size, lengthSize, [&](const uint8_t *nal, size_t len) {
        const int type = nal[0] & 0x1f;
        switch (type) {
        case 7: sps = true; break;
        case 8: pps = true; break;
        case 6: { // SEI: look for a recovery point with recovery_frame_cnt == 0
            std::array<uint8_t, 256> rbsp;
            const size_t n = unescape(nal + 1, len - 1, rbsp);
            size_t p = 0;
            while (p + 2 <= n && rbsp[p] != 0x80) {
                uint32_t payloadType = 0, payloadSize = 0;
                while (p < n && rbsp[p] == 0xff)
                    payloadType += rbsp[p++];
                if (p >= n)
                    break;
                payloadType += rbsp[p++];
                while (p < n && rbsp[p] == 0xff)
                    payloadSize += rbsp[p++];
                if (p >= n)
                    break;
                payloadSize += rbsp[p++];
                if (payloadType == 6 && p < n) {
                    BitReader br(rbsp.data() + p, n - p);
                    if (br.ue() == 0 && !br.overrun())
                        recovery = true;
                }
                p += payloadSize;
            }
            break;
        }
        case 1:
        case 5:
            if (type == 5)
                idr = true;
            if (!sliceSeen) {
                // The first slice decides: with field coding an I field is
                // often paired with a P field predicted from it, which is
                // still a valid entry point.
                sliceSeen = true;
                std::array<uint8_t, 16> rbsp;
                const size_t n = unescape(nal + 1, len - 1, rbsp);
                BitReader br(rbsp.data(), n);
                br.ue(); // first_mb_in_slice
                const uint32_t sliceType = br.ue() % 5;
                if (!br.overrun())
                    c.intra = sliceType == 2 || sliceType == 4; // I or SI
            }
            break;
        default: break;
        }
        return true;
    });

    if (idr)
        c.intra = true;
    c.randomAccess = idr || (recovery && c.intra);
    c.paramSets = sps && pps;
    return c;
}

FrameClass classifyHevc(const uint8_t *data, size_t size, int lengthSize)
{
    FrameClass c;
    c.known = true;
    bool vps = false, sps = false, pps = false, sliceSeen = false;

    forEachNal(data, size, lengthSize, [&](const uint8_t *nal, size_t len) {
        if (len < 2)
            return true;
        const int type = (nal[0] >> 1) & 0x3f;
        if (type == 32)
            vps = true;
        else if (type == 33)
            sps = true;
        else if (type == 34)
            pps = true;
        else if (type <= 31 && !sliceSeen) {
            sliceSeen = true;
            // IRAP pictures: BLA (16-18), IDR (19-20), CRA (21), reserved IRAP (22-23).
            if (type >= 16 && type <= 23) {
                c.randomAccess = true;
                c.intra = true;
            }
        }
        return true;
    });

    c.paramSets = vps && sps && pps;
    return c;
}

FrameClass classifyMpeg12(const uint8_t *data, size_t size)
{
    FrameClass c;
    c.known = true;
    for (size_t i = 0; i + 6 <= size; ++i) {
        if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1)
            continue;
        const uint8_t code = data[i + 3];
        if (code == 0xb3) {
            c.paramSets = true; // sequence header
        } else if (code == 0x00) {
            // picture header: temporal_reference (10 bits), picture_coding_type (3 bits)
            const int pictureType = (data[i + 5] >> 3) & 7;
            c.intra = pictureType == 1;
            c.randomAccess = c.intra;
            break; // only the first picture (field) matters
        }
        i += 3;
    }
    return c;
}

} // namespace

int nalLengthSize(AVCodecID codec, const uint8_t *extradata, int extradataSize)
{
    if (!extradata || extradataSize < 1 || extradata[0] != 1)
        return 0; // Annex B (or no extradata at all)
    if (codec == AV_CODEC_ID_H264 && extradataSize >= 7)
        return (extradata[4] & 3) + 1;
    if (codec == AV_CODEC_ID_HEVC && extradataSize >= 23)
        return (extradata[21] & 3) + 1;
    return 0;
}

FrameClass classifyVideoPacket(AVCodecID codec, const uint8_t *data, size_t size, int lengthSize)
{
    if (!data || size == 0)
        return {};
    switch (codec) {
    case AV_CODEC_ID_H264: return classifyH264(data, size, lengthSize);
    case AV_CODEC_ID_HEVC: return classifyHevc(data, size, lengthSize);
    case AV_CODEC_ID_MPEG1VIDEO:
    case AV_CODEC_ID_MPEG2VIDEO: return classifyMpeg12(data, size);
    default: return {};
    }
}

} // namespace adscutter
