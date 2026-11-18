// Unit tests for the cut list model and its JSON file format.

#include "model/CutList.h"

#include <QTemporaryDir>
#include <cstdio>
#include <vector>

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

using Segs = std::vector<Segment>;

// 300 frames, a cut point every 25 frames (0, 25, 50, ...).
CutList makeList()
{
    std::vector<uint32_t> keys;
    for (uint32_t k = 0; k < 300; k += 25)
        keys.push_back(k);
    CutList list;
    list.setFrames(300, keys);
    return list;
}

void testMarks()
{
    CutList l = makeList();
    CHECK(!l.isModified());
    CHECK(l.snapIn(0) == 0 && l.snapIn(24) == 0 && l.snapIn(25) == 25 && l.snapIn(299) == 275);

    // In snaps back to the cut point; out is exact.
    CHECK(l.markIn(30) == CutList::MarkResult::Pending);
    CHECK(l.pendingIn() == 25u);
    CHECK(l.markOut(70) == CutList::MarkResult::Added);
    CHECK((l.segments() == Segs{{25, 70}}));
    CHECK(!l.pendingIn() && !l.pendingOut());
    CHECK(l.isModified());

    // Out first, then in.
    CHECK(l.markOut(140) == CutList::MarkResult::Pending);
    CHECK(l.markIn(110) == CutList::MarkResult::Added);
    CHECK((l.segments() == Segs{{25, 70}, {100, 140}}));

    // Wrong order: the stale mark is dropped, the new one stays pending.
    CHECK(l.markIn(200) == CutList::MarkResult::Pending);
    CHECK(l.markOut(150) == CutList::MarkResult::Invalid);
    CHECK(!l.pendingIn() && l.pendingOut() == 150u);
    l.clearMarks();

    // Out of range.
    CHECK(l.markIn(300) == CutList::MarkResult::Invalid);
    CHECK(l.keptFrames() == 46 + 41);
}

void testMerge()
{
    CutList l = makeList();
    l.addSegment(25, 60);
    l.addSegment(100, 120);
    // Overlapping both: merges into one.
    l.addSegment(55, 105);
    CHECK((l.segments() == Segs{{25, 120}}));
    // Same start, longer: extends the existing segment.
    l.addSegment(150, 160);
    l.addSegment(150, 170);
    CHECK((l.segments() == Segs{{25, 120}, {150, 170}}));
    // 175 is a cut point after a gap (171-174): a separate segment.
    l.addSegment(175, 180);
    CHECK((l.segments() == Segs{{25, 120}, {150, 170}, {175, 180}}));
    // Touching segments (out + 1 == in) are merged.
    CutList t = makeList();
    t.addSegment(0, 49);
    t.addSegment(50, 60);
    CHECK((t.segments() == Segs{{0, 60}}));
    l.addSegment(250, 299);
    CHECK(l.segmentAt(24) == -1 && l.segmentAt(25) == 0 && l.segmentAt(120) == 0 && l.segmentAt(121) == -1);
    CHECK(l.segmentAt(178) == 2 && l.segmentAt(299) == 3);

    // Clamping of out to the last frame.
    CutList m = makeList();
    m.addSegment(10, 1000);
    CHECK((m.segments() == Segs{{0, 299}}));

    // setSegments normalizes.
    m.setSegments({{60, 80}, {30, 40}, {35, 55}});
    CHECK((m.segments() == Segs{{25, 80}}));
}

void testEdges()
{
    CutList l = makeList();
    l.addSegment(25, 60);
    l.addSegment(100, 140);
    l.addSegment(200, 220);

    // In snaps to cut points and cannot pass the out.
    CHECK(l.moveIn(1, 130) == 125);
    CHECK(l.moveIn(1, 160) == 125); // 140 snaps to 125: unchanged
    CHECK(l.moveIn(1, 76) == 75);
    // Can't overlap the previous segment (ends at 60): 55 snaps to 50, which
    // is inside it, so the first cut point after 60 is used.
    CHECK(l.moveIn(1, 55) == 75);
    CHECK(l.moveIn(0, 3) == 0);

    // Out is exact, clamped between in and the next segment.
    CHECK(l.moveOut(1, 150) == 150);
    CHECK(l.moveOut(1, 500) == 199);
    CHECK(l.moveOut(1, 10) == 75);
    CHECK(l.moveOut(2, 1000) == 299);
    CHECK((l.segments() == Segs{{0, 60}, {75, 75}, {200, 299}}));

    const auto b = l.boundaries();
    CHECK((b == std::vector<uint32_t>{0, 60, 75, 200, 299}));
}

void testUndo()
{
    CutList l = makeList();
    l.checkpoint();
    l.addSegment(25, 60);
    l.checkpoint();
    l.addSegment(100, 140);
    CHECK(l.canUndo() && !l.canRedo());

    CHECK(l.undo());
    CHECK((l.segments() == Segs{{25, 60}}));
    CHECK(l.undo());
    CHECK(l.segments().empty());
    CHECK(!l.isModified()); // back to the saved state
    CHECK(!l.undo());
    CHECK(l.redo() && l.redo());
    CHECK((l.segments() == Segs{{25, 60}, {100, 140}}));
    CHECK(l.isModified());

    l.markSaved();
    CHECK(!l.isModified());
    // A checkpoint without a change (click on an edge) is skipped by undo.
    l.checkpoint();
    l.checkpoint();
    l.moveOut(1, 150);
    CHECK(l.undo());
    CHECK((l.segments() == Segs{{25, 60}, {100, 140}}));
    CHECK(!l.isModified());
    // A new edit clears redo.
    l.checkpoint();
    l.removeSegment(0);
    CHECK(!l.canRedo());
}

void testJson()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("rec.adscutter.json"));
    CutListDocument doc;
    doc.mediaFile = QStringLiteral("rec.ts");
    doc.mediaSize = 123456789;
    doc.videoStream = 0;
    doc.frameCount = 300;
    doc.selectedStreams = {0, 2};
    doc.segments = {{{25, 60}, 1000000, 2400000}, {{100, 140}, 4000000, 5600000}};
    QString error;
    CHECK(saveCutListDocument(path, doc, &error));

    const auto loaded = loadCutListDocument(path, &error);
    CHECK(loaded.has_value());
    if (loaded) {
        CHECK(loaded->mediaFile == doc.mediaFile && loaded->mediaSize == doc.mediaSize);
        CHECK(loaded->videoStream == 0 && loaded->frameCount == 300);
        CHECK(loaded->selectedStreams == doc.selectedStreams);
        CHECK(loaded->segments.size() == 2);
        CHECK(loaded->segments[1].frames == (Segment{100, 140}));
        CHECK(loaded->segments[1].inTimeUs == 4000000 && loaded->segments[1].outTimeUs == 5600000);
    }

    QFile bad(dir.filePath(QStringLiteral("bad.json")));
    CHECK(bad.open(QIODevice::WriteOnly));
    bad.write(R"({"format":"something-else","version":1})");
    bad.close();
    CHECK(!loadCutListDocument(bad.fileName(), &error).has_value());

    CHECK(defaultCutListPath(QStringLiteral("/rec/My Show.2026-11-18.ts"))
          == QStringLiteral("/rec/My Show.2026-11-18.adscutter.json"));
}

} // namespace

int main()
{
    testMarks();
    testMerge();
    testEdges();
    testUndo();
    testJson();
    if (failures == 0)
        std::printf("all cut list tests passed\n");
    return failures == 0 ? 0 : 1;
}
