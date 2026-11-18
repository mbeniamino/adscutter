#pragma once

#include <QString>
#include <cstdint>
#include <optional>
#include <vector>

namespace adscutter {

// A part of the recording to keep, in display positions of the main video
// stream (both ends inclusive). `in` is always a cut point.
struct Segment {
    uint32_t in = 0;
    uint32_t out = 0;
    bool operator==(const Segment &) const = default;
};

// The list of kept segments, sorted and non-overlapping, with pending in/out
// marks and undo/redo.
//
// Edits snap the start of a segment to the cut point at or before the
// requested frame, because a clip can only start where decoding can start.
class CutList {
public:
    // Constraints from the index: number of frames and sorted cut points
    // (the first must be 0). Clears the list.
    void setFrames(uint32_t frameCount, std::vector<uint32_t> keyframes);
    uint32_t frameCount() const { return m_frameCount; }
    bool isEmpty() const { return m_segments.empty(); }

    const std::vector<Segment> &segments() const { return m_segments; }
    // Replaces all segments (e.g. loaded from a file). Ins are snapped,
    // overlapping segments merged, out-of-range ones clamped/dropped.
    void setSegments(const std::vector<Segment> &segments);

    uint32_t snapIn(uint32_t pos) const;

    // Marks. When both are set and in <= out, a segment is added and both
    // marks are cleared. If the new mark is on the wrong side of the other
    // one, the other one is dropped and Invalid is returned (the new mark
    // stays pending).
    enum class MarkResult { Pending, Added, Invalid };
    MarkResult markIn(uint32_t pos);
    MarkResult markOut(uint32_t pos);
    std::optional<uint32_t> pendingIn() const { return m_pendingIn; }
    std::optional<uint32_t> pendingOut() const { return m_pendingOut; }
    void clearMarks();

    // Adds [snapIn(in), out], merging with overlapping/adjacent segments.
    bool addSegment(uint32_t in, uint32_t out);
    bool removeSegment(size_t index);
    void clear();
    // Index of the segment containing pos, or -1.
    int segmentAt(uint32_t pos) const;

    // Move one edge of a segment, keeping it non-empty and not overlapping
    // its neighbours. Returns the value actually applied.
    uint32_t moveIn(size_t index, uint32_t pos);
    uint32_t moveOut(size_t index, uint32_t pos);

    uint64_t keptFrames() const;
    // All segment ins and outs, sorted, for "next/previous boundary".
    std::vector<uint32_t> boundaries() const;

    // Undo: call checkpoint() before a user edit (or a group of edits, like
    // a whole edge drag).
    void checkpoint();
    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }
    bool undo();
    bool redo();

    // Modified since the last markSaved() (or setFrames()).
    bool isModified() const { return m_revision != m_savedRevision; }
    void markSaved() { m_savedRevision = m_revision; }

private:
    struct State {
        std::vector<Segment> segments;
        uint64_t revision;
    };
    void changed() { m_revision = ++m_revisionCounter; }
    uint32_t keyframeAtOrAfter(uint32_t pos) const;

    uint32_t m_frameCount = 0;
    std::vector<uint32_t> m_keyframes{0};
    std::vector<Segment> m_segments;
    std::optional<uint32_t> m_pendingIn, m_pendingOut;
    std::vector<State> m_undo, m_redo;
    uint64_t m_revision = 0, m_savedRevision = 0, m_revisionCounter = 0;
};

// On-disk cut list (JSON).
struct CutListDocument {
    QString mediaFile;     // file name of the recording, informational
    int64_t mediaSize = -1;
    int videoStream = -1;
    uint32_t frameCount = 0;
    std::vector<int> selectedStreams;
    struct Entry {
        Segment frames;
        int64_t inTimeUs = 0; // unified time of the first frame
        int64_t outTimeUs = 0; // unified time of the last frame
    };
    std::vector<Entry> segments;
};

bool saveCutListDocument(const QString &path, const CutListDocument &doc, QString *error);
std::optional<CutListDocument> loadCutListDocument(const QString &path, QString *error);
// <recording without extension>.adscutter.json
QString defaultCutListPath(const QString &mediaPath);

} // namespace adscutter
