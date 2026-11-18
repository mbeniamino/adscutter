#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace adscutter {

// Makes H.264 segments that start at a non-IDR cut point (an I picture with
// a recovery point, typical of broadcast open GOPs) decodable after a splice.
//
// Without it a decoder keeps the previous segment's reference pictures and
// picture order: pictures right after the cut predict from the wrong frames
// and come out in the wrong order. The fix used by splicers:
//  - the cut point's slices become IDR slices (frame_num 0, POC lsb 0, an
//    idr_pic_id and IDR reference marking), which flushes decoder state;
//  - frame_num and the POC lsb of every following slice, up to the next
//    real IDR, are shifted by the same amounts so numbering stays
//    continuous.
//  - reference marking commands (MMCO) of following slices that refer to
//    pictures before the cut are removed.
// Only slice headers are touched; slice data is copied bit for bit.
class H264Splicer {
public:
    explicit H264Splicer(int nalLengthSize) : m_nalLength(nalLengthSize) {}

    // Learns SPS/PPS from avcC or Annex B extradata, or from packet data.
    void addExtradata(const uint8_t *data, size_t size);
    void addParameterSets(const uint8_t *data, size_t size);

    // Rewrites the packet of a segment's cut point. Returns false (and
    // leaves `out` untouched) if the packet needs no change or cannot be
    // converted; `why` then says why if it was a failure.
    bool startSegment(const uint8_t *data, size_t size, int idrPicId, std::vector<uint8_t> &out, std::string *why);
    // Rewrites the following packets of the segment. Returns false if the
    // packet is unchanged.
    bool continueSegment(const uint8_t *data, size_t size, std::vector<uint8_t> &out);
    bool active() const { return m_active; }

private:
    struct Sps {
        int log2MaxFrameNum = 4;
        int pocType = 0;
        int log2MaxPocLsb = 4;
        bool deltaPicOrderAlwaysZero = false;
        bool frameMbsOnly = true;
        bool separateColourPlane = false;
        int chromaArrayType = 1;
    };
    struct Pps {
        int spsId = 0;
        bool cabac = false;
        bool bottomFieldPicOrderPresent = false;
        bool redundantPicCntPresent = false;
        bool deblockingFilterControlPresent = false;
        bool sliceGroups = false;
        uint32_t numRefIdxL0 = 1, numRefIdxL1 = 1; // defaults (active = minus1 + 1)
        bool weightedPred = false;
        uint32_t weightedBipredIdc = 0;
    };

    void parseNal(const uint8_t *nal, size_t len);
    // Rewrites one slice NAL into `payload` (escaped, without the NAL
    // header byte, which goes to `header`). Returns false if unchanged or
    // unsupported (then `why` is set).
    bool rewriteSlice(const uint8_t *nal, size_t len, bool makeIdr, int idrPicId, std::vector<uint8_t> &payload,
                      uint8_t &header, std::string *why, bool forceFull = false);
    void appendNal(std::vector<uint8_t> &out, uint8_t header, const std::vector<uint8_t> &payload) const;

    int m_nalLength;
    std::map<int, Sps> m_sps;
    std::map<int, Pps> m_pps;
    bool m_active = false;   // following slices need their frame_num shifted
    uint32_t m_frameNumShift = 0;
    uint32_t m_pocLsbShift = 0;
    // Reference frames decoded since the (converted) IDR, including it:
    // reference marking commands reaching further back refer to pictures
    // that were cut away and are dropped.
    uint32_t m_refsSinceIdr = 0;
    int m_log2MaxFrameNum = 4;
};

} // namespace adscutter
