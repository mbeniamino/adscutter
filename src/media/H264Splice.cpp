#include "media/H264Splice.h"

#include <algorithm>
#include <iterator>

namespace adscutter {

namespace {

class Reader {
public:
    Reader(const std::vector<uint8_t> &d)
        : m_d(d)
    {
    }
    size_t pos() const { return m_pos; }
    bool overrun() const { return m_pos > m_d.size() * 8; }
    uint32_t bit()
    {
        const uint32_t b = m_pos < m_d.size() * 8 ? (m_d[m_pos >> 3] >> (7 - (m_pos & 7))) & 1 : 0;
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
    uint32_t ue()
    {
        int zeros = 0;
        while (bit() == 0) {
            if (++zeros > 31 || overrun()) {
                m_pos = m_d.size() * 8 + 1;
                return 0;
            }
        }
        return ((1u << zeros) - 1) + bits(zeros);
    }
    int32_t se()
    {
        const uint32_t v = ue();
        return (v & 1) ? int32_t((v + 1) / 2) : -int32_t(v / 2);
    }

private:
    const std::vector<uint8_t> &m_d;
    size_t m_pos = 0;
};

class Writer {
public:
    std::vector<uint8_t> data;
    size_t bitsWritten() const { return m_bits; }
    void bit(uint32_t b)
    {
        if ((m_bits & 7) == 0)
            data.push_back(0);
        if (b)
            data.back() |= uint8_t(0x80 >> (m_bits & 7));
        ++m_bits;
    }
    void bits(uint32_t v, int n)
    {
        while (n-- > 0)
            bit((v >> n) & 1);
    }
    void ue(uint32_t v)
    {
        const uint64_t x = uint64_t(v) + 1;
        int len = 0;
        while ((x >> len) > 1)
            ++len;
        bits(0, len);
        for (int i = len; i >= 0; --i)
            bit(uint32_t(x >> i) & 1);
    }
    // Copies bits [from, to) of src.
    void copy(const std::vector<uint8_t> &src, size_t from, size_t to)
    {
        for (size_t p = from; p < to; ++p)
            bit((src[p >> 3] >> (7 - (p & 7))) & 1);
    }
    bool aligned() const { return (m_bits & 7) == 0; }

private:
    size_t m_bits = 0;
};

std::vector<uint8_t> unescape(const uint8_t *src, size_t size)
{
    std::vector<uint8_t> out;
    out.reserve(size);
    int zeros = 0;
    for (size_t i = 0; i < size; ++i) {
        if (zeros >= 2 && src[i] == 3) {
            zeros = 0;
            continue;
        }
        zeros = src[i] == 0 ? zeros + 1 : 0;
        out.push_back(src[i]);
    }
    return out;
}

// Unescapes at least `want` RBSP bytes (or all of them), stopping right
// after a non-zero source byte so that the rest of the source can later be
// appended unchanged after a re-escaped, modified prefix. *consumed is the
// number of source bytes used.
std::vector<uint8_t> unescapePrefix(const uint8_t *src, size_t size, size_t want, size_t *consumed)
{
    std::vector<uint8_t> out;
    out.reserve(std::min(size, want + 16));
    int zeros = 0;
    size_t i = 0;
    for (; i < size; ++i) {
        if (out.size() >= want && src[i - 1] != 0)
            break;
        if (zeros >= 2 && src[i] == 3) {
            zeros = 0;
            continue;
        }
        zeros = src[i] == 0 ? zeros + 1 : 0;
        out.push_back(src[i]);
    }
    *consumed = i;
    return out;
}

// Adds emulation prevention bytes to an RBSP. `complete`: the RBSP ends the
// NAL unit (a final zero byte then needs protection too).
std::vector<uint8_t> escape(const std::vector<uint8_t> &rbsp, bool complete)
{
    std::vector<uint8_t> out;
    out.reserve(rbsp.size() + rbsp.size() / 64 + 2);
    int zeros = 0;
    for (uint8_t b : rbsp) {
        if (zeros >= 2 && b <= 3) {
            out.push_back(3);
            zeros = 0;
        }
        out.push_back(b);
        zeros = b == 0 ? zeros + 1 : 0;
    }
    if (complete && !out.empty() && out.back() == 0)
        out.push_back(3);
    return out;
}

template <typename F>
void forEachNal(const uint8_t *data, size_t size, int lengthSize, F &&f)
{
    if (lengthSize > 0) {
        size_t pos = 0;
        while (pos + size_t(lengthSize) <= size) {
            size_t len = 0;
            for (int i = 0; i < lengthSize; ++i)
                len = (len << 8) | data[pos + i];
            pos += size_t(lengthSize);
            if (len == 0 || len > size - pos)
                break;
            f(data + pos, len);
            pos += len;
        }
        return;
    }
    auto findStart = [&](size_t from) -> size_t {
        for (size_t i = from; i + 3 <= size; ++i)
            if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
                return i;
        return size;
    };
    size_t start = findStart(0);
    while (start < size) {
        const size_t nal = start + 3;
        const size_t next = findStart(nal);
        size_t end = next;
        while (end > nal && data[end - 1] == 0)
            --end;
        if (end > nal)
            f(data + nal, end - nal);
        start = next;
    }
}

void skipScalingList(Reader &r, int size)
{
    int last = 8, next = 8;
    for (int j = 0; j < size; ++j) {
        if (next != 0)
            next = (last + r.se() + 256) % 256;
        last = next == 0 ? last : next;
    }
}

} // namespace

void H264Splicer::addExtradata(const uint8_t *data, size_t size)
{
    if (!data || size == 0)
        return;
    if (data[0] != 1) { // Annex B
        addParameterSets(data, size);
        return;
    }
    // avcC: version, profile, compat, level, lengthSize, numSps, [len sps]..., numPps, [len pps]...
    if (size < 7)
        return;
    size_t p = 5;
    for (int list = 0; list < 2 && p < size; ++list) {
        const int count = list == 0 ? (data[p] & 0x1f) : data[p];
        ++p;
        for (int i = 0; i < count && p + 2 <= size; ++i) {
            const size_t len = size_t(data[p] << 8 | data[p + 1]);
            p += 2;
            if (p + len > size)
                return;
            parseNal(data + p, len);
            p += len;
        }
    }
}

void H264Splicer::addParameterSets(const uint8_t *data, size_t size)
{
    forEachNal(data, size, data && size > 0 && data[0] == 0 ? 0 : m_nalLength,
               [&](const uint8_t *nal, size_t len) { parseNal(nal, len); });
}

void H264Splicer::parseNal(const uint8_t *nal, size_t len)
{
    if (len < 2)
        return;
    const int type = nal[0] & 0x1f;
    if (type != 7 && type != 8)
        return;
    const std::vector<uint8_t> rbsp = unescape(nal + 1, len - 1);
    Reader r(rbsp);
    if (type == 7) {
        Sps sps;
        const uint32_t profile = r.bits(8);
        r.bits(16); // constraint flags, level
        const uint32_t id = r.ue();
        static const uint32_t highProfiles[] = {100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135};
        if (std::find(std::begin(highProfiles), std::end(highProfiles), profile) != std::end(highProfiles)) {
            const uint32_t chroma = r.ue();
            if (chroma == 3)
                sps.separateColourPlane = r.bit();
            sps.chromaArrayType = sps.separateColourPlane ? 0 : int(chroma);
            r.ue(); // bit depth luma
            r.ue(); // bit depth chroma
            r.bit(); // qpprime_y_zero_transform_bypass
            if (r.bit()) { // seq_scaling_matrix_present
                for (int i = 0; i < (chroma == 3 ? 12 : 8); ++i)
                    if (r.bit())
                        skipScalingList(r, i < 6 ? 16 : 64);
            }
        }
        sps.log2MaxFrameNum = int(r.ue()) + 4;
        sps.pocType = int(r.ue());
        if (sps.pocType == 0) {
            sps.log2MaxPocLsb = int(r.ue()) + 4;
        } else if (sps.pocType == 1) {
            sps.deltaPicOrderAlwaysZero = r.bit();
            r.se();
            r.se();
            const uint32_t n = r.ue();
            for (uint32_t i = 0; i < n && !r.overrun(); ++i)
                r.se();
        }
        r.ue(); // max_num_ref_frames
        r.bit(); // gaps_in_frame_num_value_allowed
        r.ue(); // width
        r.ue(); // height
        sps.frameMbsOnly = r.bit();
        if (!r.overrun() && id < 32 && sps.log2MaxFrameNum <= 16)
            m_sps[int(id)] = sps;
    } else {
        Pps pps;
        const uint32_t id = r.ue();
        pps.spsId = int(r.ue());
        pps.cabac = r.bit();
        pps.bottomFieldPicOrderPresent = r.bit();
        pps.sliceGroups = r.ue() > 0;
        if (!pps.sliceGroups) {
            pps.numRefIdxL0 = r.ue() + 1;
            pps.numRefIdxL1 = r.ue() + 1;
            pps.weightedPred = r.bit();
            pps.weightedBipredIdc = r.bits(2);
            r.se(); // pic_init_qp
            r.se(); // pic_init_qs
            r.se(); // chroma_qp_index_offset
            pps.deblockingFilterControlPresent = r.bit();
            r.bit(); // constrained_intra_pred
            pps.redundantPicCntPresent = r.bit();
        }
        if (!r.overrun() && id < 256)
            m_pps[int(id)] = pps;
    }
}

void H264Splicer::appendNal(std::vector<uint8_t> &out, uint8_t header, const std::vector<uint8_t> &payload) const
{
    const size_t size = payload.size() + 1;
    if (m_nalLength > 0) {
        for (int b = m_nalLength - 1; b >= 0; --b)
            out.push_back(uint8_t(size >> (8 * b)));
    } else {
        out.insert(out.end(), {0, 0, 0, 1});
    }
    out.push_back(header);
    out.insert(out.end(), payload.begin(), payload.end());
}

bool H264Splicer::rewriteSlice(const uint8_t *nal, size_t len, bool makeIdr, int idrPicId,
                               std::vector<uint8_t> &out, uint8_t &header, std::string *why, bool forceFull)
{
    auto fail = [&](const char *reason) {
        if (why)
            *why = reason;
        return false;
    };
    header = nal[0];
    const int nalType = nal[0] & 0x1f;
    const int refIdc = (nal[0] >> 5) & 3;
    // Slice headers are short: work on an unescaped prefix and keep the
    // (large) slice data as it is, unless the header changes length.
    size_t consumed = 0;
    std::vector<uint8_t> rbsp = forceFull ? unescape(nal + 1, len - 1)
                                          : unescapePrefix(nal + 1, len - 1, 256, &consumed);
    const bool complete = forceFull || consumed == len - 1;
    Reader r(rbsp);

    // --- Parse the whole slice header, remembering where fields are -----------
    r.ue(); // first_mb_in_slice
    const uint32_t sliceType = r.ue() % 5;
    const bool isP = sliceType == 0 || sliceType == 3;
    const bool isB = sliceType == 1;
    const bool isI = sliceType == 2 || sliceType == 4;
    auto ppsIt = m_pps.find(int(r.ue()));
    if (ppsIt == m_pps.end())
        return fail("slice refers to an unknown PPS");
    const Pps &pps = ppsIt->second;
    auto spsIt = m_sps.find(pps.spsId);
    if (spsIt == m_sps.end())
        return fail("slice refers to an unknown SPS");
    const Sps &sps = spsIt->second;
    if (pps.sliceGroups)
        return fail("slice groups are not supported");
    if (sps.separateColourPlane)
        r.bits(2);
    const size_t frameNumPos = r.pos();
    const uint32_t frameNum = r.bits(sps.log2MaxFrameNum);
    const size_t afterFrameNum = r.pos();
    bool fieldPic = false;
    if (!sps.frameMbsOnly) {
        fieldPic = r.bit();
        if (fieldPic)
            r.bit(); // bottom_field_flag
    }
    const size_t idrIdPos = r.pos();
    if (nalType == 5)
        r.ue(); // idr_pic_id
    const size_t pocPos = r.pos();
    uint32_t pocLsb = 0;
    if (sps.pocType == 0) {
        pocLsb = r.bits(sps.log2MaxPocLsb);
        if (pps.bottomFieldPicOrderPresent && !fieldPic)
            r.se();
    } else if (sps.pocType == 1 && !sps.deltaPicOrderAlwaysZero) {
        r.se();
        if (pps.bottomFieldPicOrderPresent && !fieldPic)
            r.se();
    }
    if (pps.redundantPicCntPresent)
        r.ue();
    if (isB)
        r.bit(); // direct_spatial_mv_pred_flag
    uint32_t numL0 = pps.numRefIdxL0, numL1 = pps.numRefIdxL1;
    if ((isP || isB) && r.bit()) { // num_ref_idx_active_override_flag
        numL0 = r.ue() + 1;
        if (isB)
            numL1 = r.ue() + 1;
    }
    auto listModification = [&] {
        if (r.bit()) {
            for (;;) {
                const uint32_t idc = r.ue();
                if (idc == 3 || r.overrun())
                    break;
                r.ue();
            }
        }
    };
    if (!isI)
        listModification();
    if (isB)
        listModification();
    if ((pps.weightedPred && isP) || (pps.weightedBipredIdc == 1 && isB)) {
        r.ue(); // luma_log2_weight_denom
        if (sps.chromaArrayType != 0)
            r.ue();
        auto table = [&](uint32_t n) {
            for (uint32_t i = 0; i < n && !r.overrun(); ++i) {
                if (r.bit()) {
                    r.se();
                    r.se();
                }
                if (sps.chromaArrayType != 0 && r.bit()) {
                    for (int k = 0; k < 4; ++k)
                        r.se();
                }
            }
        };
        table(numL0);
        if (isB)
            table(numL1);
    }
    const size_t markingStart = r.pos();
    struct Mmco {
        uint32_t op, a = 0, b = 0;
    };
    std::vector<Mmco> mmcos;
    bool adaptive = false;
    if (refIdc != 0) {
        if (nalType == 5) {
            r.bits(2);
        } else if ((adaptive = r.bit())) {
            for (;;) {
                Mmco m{r.ue()};
                if (m.op == 0 || r.overrun())
                    break;
                if (m.op == 1 || m.op == 2 || m.op == 3 || m.op == 6 || m.op == 4)
                    m.a = r.ue();
                if (m.op == 3)
                    m.b = r.ue();
                mmcos.push_back(m);
            }
        }
    }
    const size_t markingEnd = r.pos();
    if (pps.cabac && !isI)
        r.ue(); // cabac_init_idc
    r.se(); // slice_qp_delta
    if (sliceType == 3) {
        r.bit(); // sp_for_switch_flag
        r.se();
    } else if (sliceType == 4) {
        r.se(); // slice_qs_delta
    }
    if (pps.deblockingFilterControlPresent && r.ue() != 1) {
        r.se();
        r.se();
    }
    const size_t headerEnd = r.pos();
    if (headerEnd + 8 > rbsp.size() * 8 && !complete) {
        // The header did not fit in the prefix: start over on all the data.
        return rewriteSlice(nal, len, makeIdr, idrPicId, out, header, why, true);
    }
    if (r.overrun())
        return fail("malformed slice header");

    // --- New values ----------------------------------------------------------------
    if (makeIdr && !isI)
        return fail("cut point slice is not intra");
    if (makeIdr && sps.pocType == 1)
        return fail("picture order count type 1 is not supported");
    const uint32_t frameMask = (1u << sps.log2MaxFrameNum) - 1;
    const uint32_t lsbMask = (1u << sps.log2MaxPocLsb) - 1;
    const uint32_t newFrameNum = makeIdr ? 0 : (frameNum - m_frameNumShift) & frameMask;
    const uint32_t newLsb = makeIdr ? 0 : (pocLsb - m_pocLsbShift) & lsbMask;

    // Drop marking commands aimed at pictures before the cut (frame coding:
    // each reference frame since the IDR advanced the picture number by one).
    bool markingChanged = makeIdr;
    std::vector<Mmco> kept;
    if (!makeIdr && adaptive && !fieldPic) {
        for (const Mmco &m : mmcos) {
            if ((m.op == 1 || m.op == 3) && m.a + 1 > m_refsSinceIdr) {
                markingChanged = true;
                continue;
            }
            kept.push_back(m);
        }
    }
    if (!makeIdr && newFrameNum == frameNum && newLsb == pocLsb && !markingChanged)
        return false;

    // --- Write ------------------------------------------------------------------------
    Writer w;
    w.copy(rbsp, 0, frameNumPos);
    w.bits(newFrameNum, sps.log2MaxFrameNum);
    w.copy(rbsp, afterFrameNum, idrIdPos);
    if (makeIdr)
        w.ue(uint32_t(idrPicId));
    else
        w.copy(rbsp, idrIdPos, pocPos);
    if (sps.pocType == 0) {
        // An IDR picture must have a picture order count of 0 (8.2.1).
        w.bits(newLsb, sps.log2MaxPocLsb);
        w.copy(rbsp, pocPos + size_t(sps.log2MaxPocLsb), markingStart);
    } else {
        w.copy(rbsp, pocPos, markingStart);
    }

    if (!markingChanged) {
        // Same length as before: the rest of the prefix is copied as is and
        // the remaining source bytes are appended untouched.
        w.copy(rbsp, markingStart, rbsp.size() * 8);
        out = escape(w.data, complete);
        out.insert(out.end(), nal + 1 + consumed, nal + len);
        return true;
    }
    // The header changes length: the slice data must move too.
    if (!complete)
        return rewriteSlice(nal, len, makeIdr, idrPicId, out, header, why, true);
    if (makeIdr) {
        w.bits(0, 2); // no_output_of_prior_pics_flag, long_term_reference_flag
    } else if (kept.empty()) {
        w.bit(0); // sliding window
    } else {
        w.bit(1);
        for (const Mmco &m : kept) {
            w.ue(m.op);
            if (m.op == 1 || m.op == 2 || m.op == 3 || m.op == 6 || m.op == 4)
                w.ue(m.a);
            if (m.op == 3)
                w.ue(m.b);
        }
        w.ue(0);
    }
    w.copy(rbsp, markingEnd, headerEnd);

    // The header changed length: re-attach the slice data.
    if (pps.cabac) {
        // cabac_alignment_one_bits, then the arithmetic coded data as is.
        while (!w.aligned())
            w.bit(1);
        const size_t dataStart = (headerEnd + 7) / 8;
        w.data.insert(w.data.end(), rbsp.begin() + ptrdiff_t(dataStart), rbsp.end());
    } else {
        // CAVLC data follows the header directly: copy bits up to the stop
        // bit, then terminate again.
        size_t stop = rbsp.size() * 8;
        while (stop > headerEnd && !((rbsp[(stop - 1) >> 3] >> (7 - ((stop - 1) & 7))) & 1))
            --stop;
        if (stop <= headerEnd)
            return fail("slice without data");
        w.copy(rbsp, headerEnd, stop - 1);
        w.bit(1);
        while (!w.aligned())
            w.bit(0);
    }

    if (makeIdr) {
        header = uint8_t((std::max(refIdc, 1) << 5) | 5);
        m_frameNumShift = frameNum;
        m_pocLsbShift = pocLsb;
    }
    out = escape(w.data, true);
    return true;
}

bool H264Splicer::startSegment(const uint8_t *data, size_t size, int idrPicId, std::vector<uint8_t> &out,
                               std::string *why)
{
    m_active = false;
    m_frameNumShift = 0;
    m_pocLsbShift = 0;

    // Already an IDR, or no slice at all: nothing to do.
    bool isIdr = false, haveSlice = false;
    forEachNal(data, size, m_nalLength, [&](const uint8_t *nal, size_t) {
        const int type = nal[0] & 0x1f;
        if (type == 5)
            isIdr = true;
        haveSlice |= type == 1 || type == 5;
    });
    if (isIdr || !haveSlice)
        return false;

    std::vector<uint8_t> result;
    bool firstPicture = true, sliceSeen = false, ok = true;
    forEachNal(data, size, m_nalLength, [&](const uint8_t *nal, size_t len) {
        if (!ok)
            return;
        const int type = nal[0] & 0x1f;
        if (type == 7 || type == 8)
            parseNal(nal, len);
        if (type != 1) {
            // Copy as is.
            if (m_nalLength > 0) {
                for (int b = m_nalLength - 1; b >= 0; --b)
                    result.push_back(uint8_t(len >> (8 * b)));
            } else {
                result.insert(result.end(), {0, 0, 0, 1});
            }
            result.insert(result.end(), nal, nal + len);
            return;
        }
        // A new picture starts with first_mb_in_slice == 0 (e.g. the
        // second field of an I/P field pair): only the first one becomes IDR.
        if (sliceSeen && len > 1 && (nal[1] & 0x80))
            firstPicture = false;
        sliceSeen = true;
        std::vector<uint8_t> rbsp;
        uint8_t header = 0;
        if (firstPicture) {
            if (!rewriteSlice(nal, len, true, idrPicId, rbsp, header, why)) {
                ok = false;
                return;
            }
        } else if (!rewriteSlice(nal, len, false, 0, rbsp, header, why)) {
            rbsp.assign(nal + 1, nal + len); // frame_num already right
            header = nal[0];
        }
        appendNal(result, header, rbsp);
    });
    if (!ok)
        return false;
    m_active = true; // following slices may need numbering or marking fixes
    m_refsSinceIdr = 1;
    out = std::move(result);
    return true;
}

bool H264Splicer::continueSegment(const uint8_t *data, size_t size, std::vector<uint8_t> &out)
{
    if (!m_active)
        return false;
    bool idr = false;
    forEachNal(data, size, m_nalLength, [&](const uint8_t *nal, size_t) { idr |= (nal[0] & 0x1f) == 5; });
    if (idr) {
        m_active = false; // a real IDR restarts numbering
        return false;
    }

    std::vector<uint8_t> result;
    bool changed = false, reference = false;
    forEachNal(data, size, m_nalLength, [&](const uint8_t *nal, size_t len) {
        const int type = nal[0] & 0x1f;
        if (type == 7 || type == 8)
            parseNal(nal, len);
        if (type == 1 && (nal[0] & 0x60))
            reference = true;
        std::vector<uint8_t> rbsp;
        uint8_t header = 0;
        if (type == 1 && rewriteSlice(nal, len, false, 0, rbsp, header, nullptr)) {
            appendNal(result, header, rbsp);
            changed = true;
            return;
        }
        if (m_nalLength > 0) {
            for (int b = m_nalLength - 1; b >= 0; --b)
                result.push_back(uint8_t(len >> (8 * b)));
        } else {
            result.insert(result.end(), {0, 0, 0, 1});
        }
        result.insert(result.end(), nal, nal + len);
    });
    if (reference)
        ++m_refsSinceIdr;
    if (!changed)
        return false;
    out = std::move(result);
    return true;
}

} // namespace adscutter
