#pragma once

#include "media/PacketIndex.h"
#include "model/CutList.h"

#include <QStringList>
#include <functional>
#include <vector>

namespace adscutter {

struct ExportSettings {
    QString outputPath;
    int videoStream = -1;
    std::vector<int> streams;      // streams to write (Matroska compatible)
    std::vector<Segment> segments; // display positions of videoStream; ins must be cut points
    int64_t frameDurationUs = 40000;
};

struct ExportReport {
    struct SegmentInfo {
        Segment frames;
        // Reference frames shown after `out` that later frames of the segment
        // depend on, so they cannot be dropped without re-encoding.
        uint32_t extraFrames = 0;
        std::vector<uint32_t> extraPositions; // their display positions (not always out+1, out+2...)
        int64_t sourceStartUs = 0; // unified time of `in`
        int64_t durationUs = 0;    // including the extra frames
    };
    std::vector<SegmentInfo> segments;
    int64_t durationUs = 0;
    uint64_t packetsWritten = 0;
    uint32_t paramSetsInserted = 0;
    uint32_t blaConversions = 0;
    uint32_t idrConversions = 0;
    QStringList warnings;
};

// Writes the selected streams of the given segments to a Matroska file
// without re-encoding. progress(fraction) may return false to cancel; a
// canceled or failed export leaves no output file.
bool exportMatroska(const QString &inputPath, const PacketIndex &index, const ExportSettings &settings,
                    const std::function<bool(double)> &progress, ExportReport *report, QString *error);

} // namespace adscutter
