#pragma once

#include "model/CutList.h"

#include <cstdint>
#include <vector>

namespace adscutter {

// Frame ranges [first, last] (inclusive), sorted and disjoint.
using FrameRanges = std::vector<std::pair<uint32_t, uint32_t>>;

// The programme according to one source: a region "shown during the
// programme" gives where it is shown; one "shown during ads" and the fade
// breaks give the gaps between where they are shown.
FrameRanges complementOf(const FrameRanges &ranges, uint32_t frameCount);

// Where every source agrees it is programme.
FrameRanges intersectAll(const std::vector<FrameRanges> &sources, uint32_t frameCount);

// Moves programme boundaries onto nearby break edges (at most `tolerance`
// frames away): regions tell where the programme is, but fades are frame
// exact (a region's text may change a little after the fade). A break edge
// that is too far away is ignored: fades also occur inside programmes
// (dark opening scenes), so they cannot decide on their own when regions
// are available.
FrameRanges snapToBreaks(const FrameRanges &programme, const FrameRanges &breaks, uint32_t tolerance);

struct ProposalSettings {
    // Without re-encoding a part can only start at a cut point: the one
    // before the programme resumes (keeps up to one GOP of the break) or
    // the one after (loses up to one GOP of programme).
    enum class StartAt { CutPointBefore, CutPointAfter };
    StartAt startAt = StartAt::CutPointBefore;
    uint32_t minFrames = 250; // shorter programme pieces are dropped
};

struct Proposal {
    std::vector<Segment> segments;
    // Per segment: frames of the break kept before the programme resumes
    // (CutPointBefore) or programme frames lost (CutPointAfter).
    std::vector<uint32_t> startOffsets;
};

Proposal proposeSegments(const FrameRanges &programme, const std::vector<uint32_t> &keyframes, uint32_t frameCount,
                         const ProposalSettings &settings = {});

} // namespace adscutter
