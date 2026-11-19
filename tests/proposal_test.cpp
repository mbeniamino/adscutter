// Unit tests for building a proposed cut list from cue sources.

#include "analysis/Proposal.h"

#include <cstdio>

using namespace adscutter;

namespace {
int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)
using R = FrameRanges;
using S = std::vector<Segment>;
} // namespace

int main()
{
    // Complement.
    CHECK((complementOf({{10, 19}, {40, 49}}, 100) == R{{0, 9}, {20, 39}, {50, 99}}));
    CHECK((complementOf({{0, 99}}, 100).empty()));
    CHECK((complementOf({}, 100) == R{{0, 99}}));
    CHECK((complementOf({{0, 9}, {90, 99}}, 100) == R{{10, 89}}));

    // Intersection: every source must agree it is programme.
    const R region = {{100, 999}, {1300, 2999}};    // e.g. "PRIMA" shown
    const R fades = {{0, 989}, {1310, 3999}};      // outside the fade breaks
    CHECK((intersectAll({region, fades}, 4000) == R{{100, 989}, {1310, 2999}}));
    CHECK((intersectAll({}, 4000) == R{{0, 3999}}));
    CHECK((intersectAll({region}, 4000) == region));
    CHECK((intersectAll({{{0, 50}}, {{60, 70}}}, 100).empty()));

    // Snapping to breaks: the region's change 40 frames after the fade is
    // moved onto it; a break edge 400 frames away (dark programme opening)
    // is ignored.
    const R breaks = {{960, 1299}, {3000, 3499}};
    CHECK((snapToBreaks({{100, 999}, {1300, 2999}}, breaks, 150) == R{{100, 959}, {1300, 2999}}));
    CHECK((snapToBreaks({{0, 2600}}, breaks, 150) == R{{0, 2600}}));
    CHECK((snapToBreaks({{1350, 2960}}, breaks, 150) == R{{1300, 2999}}));

    // Cut points every 96 frames.
    std::vector<uint32_t> keys;
    for (uint32_t k = 0; k < 4000; k += 96)
        keys.push_back(k);
    const R programme = {{100, 989}, {1310, 2999}, {3100, 3150}};

    // Start at the cut point before: 96 (4 frames of the break), 1248 (62).
    Proposal before = proposeSegments(programme, keys, 4000);
    CHECK((before.segments == S{{96, 989}, {1248, 2999}}));          // 3100-3150 too short (51 < 250)
    CHECK((before.startOffsets == std::vector<uint32_t>{4, 62}));

    // Start at the cut point after: 192 (92 frames lost), 1344 (34).
    ProposalSettings after;
    after.startAt = ProposalSettings::StartAt::CutPointAfter;
    Proposal a = proposeSegments(programme, keys, 4000, after);
    CHECK((a.segments == S{{192, 989}, {1344, 2999}}));
    CHECK((a.startOffsets == std::vector<uint32_t>{92, 34}));

    // A part whose cut point before falls inside the previous one: merged.
    Proposal touching = proposeSegments({{0, 400}, {410, 900}}, keys, 4000);
    CHECK((touching.segments == S{{0, 900}}));

    // No cut point inside a programme piece (cut point after): dropped.
    ProposalSettings shortAfter = after;
    shortAfter.minFrames = 10;
    CHECK(proposeSegments({{97, 150}}, keys, 4000, shortAfter).segments.empty());

    if (failures == 0)
        std::printf("all proposal tests passed\n");
    return failures == 0 ? 0 : 1;
}
