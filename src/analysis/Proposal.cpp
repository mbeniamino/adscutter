#include "analysis/Proposal.h"

#include <algorithm>

namespace adscutter {

FrameRanges complementOf(const FrameRanges &ranges, uint32_t frameCount)
{
    FrameRanges out;
    uint32_t next = 0;
    for (const auto &[first, last] : ranges) {
        if (first > next)
            out.emplace_back(next, first - 1);
        next = std::max(next, last + 1);
    }
    if (next < frameCount)
        out.emplace_back(next, frameCount - 1);
    return out;
}

FrameRanges intersectAll(const std::vector<FrameRanges> &sources, uint32_t frameCount)
{
    FrameRanges result = frameCount > 0 ? FrameRanges{{0, frameCount - 1}} : FrameRanges{};
    for (const FrameRanges &source : sources) {
        FrameRanges merged;
        size_t i = 0, j = 0;
        while (i < result.size() && j < source.size()) {
            const uint32_t first = std::max(result[i].first, source[j].first);
            const uint32_t last = std::min(result[i].second, source[j].second);
            if (first <= last)
                merged.emplace_back(first, last);
            // Advance the range that ends first.
            if (result[i].second < source[j].second)
                ++i;
            else
                ++j;
        }
        result = std::move(merged);
    }
    return result;
}

FrameRanges snapToBreaks(const FrameRanges &programme, const FrameRanges &breaks, uint32_t tolerance)
{
    auto distance = [](uint32_t a, uint32_t b) { return a > b ? a - b : b - a; };
    FrameRanges out;
    for (auto [first, last] : programme) {
        // Programme resumes: the first frame after the nearest break.
        uint32_t bestStart = first, bestStartDistance = tolerance + 1;
        // Programme ends: the last frame before the nearest break.
        uint32_t bestEnd = last, bestEndDistance = tolerance + 1;
        for (const auto &[bFirst, bLast] : breaks) {
            if (distance(bLast + 1, first) < bestStartDistance) {
                bestStartDistance = distance(bLast + 1, first);
                bestStart = bLast + 1;
            }
            if (bFirst > 0 && distance(bFirst - 1, last) < bestEndDistance) {
                bestEndDistance = distance(bFirst - 1, last);
                bestEnd = bFirst - 1;
            }
        }
        if (bestStart <= bestEnd)
            out.emplace_back(bestStart, bestEnd);
    }
    return out;
}

Proposal proposeSegments(const FrameRanges &programme, const std::vector<uint32_t> &keyframes, uint32_t frameCount,
                         const ProposalSettings &settings)
{
    Proposal p;
    for (const auto &[first, last] : programme) {
        if (last >= frameCount || last - first + 1 < settings.minFrames)
            continue;
        uint32_t in = first, offset = 0;
        if (settings.startAt == ProposalSettings::StartAt::CutPointBefore) {
            auto it = std::upper_bound(keyframes.begin(), keyframes.end(), first);
            in = it == keyframes.begin() ? 0 : *(it - 1);
            offset = first - in;
        } else {
            auto it = std::lower_bound(keyframes.begin(), keyframes.end(), first);
            if (it == keyframes.end() || *it > last)
                continue; // no cut point inside: nothing can be kept
            in = *it;
            offset = in - first;
        }
        // A part must not start inside the previous one (cut point before).
        if (!p.segments.empty() && in <= p.segments.back().out) {
            p.segments.back().out = last; // they touch: one part
            continue;
        }
        p.segments.push_back({in, last});
        p.startOffsets.push_back(offset);
    }
    return p;
}

} // namespace adscutter
