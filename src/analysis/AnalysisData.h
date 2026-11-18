#pragma once

#include "media/PacketIndex.h"

#include <QString>
#include <cstdint>
#include <memory>
#include <vector>

namespace adscutter {

// Per-frame picture measurements, by display position of the main video
// stream. 4 bytes, so a 3 hour 50 fps recording takes about 2 MB.
struct FrameFeatures {
    uint8_t mean = 0;  // average luma (0-255)
    uint8_t dark = 0;  // fraction of near-black pixels (0-255)
    uint8_t scene = 0; // mean absolute luma change from the previous frame
    uint8_t valid = 0; // 1 once the frame was analysed
};

// Results of the analysis of a recording (see Analyzer.h).
struct AnalysisData {
    static constexpr int64_t kAudioWindowUs = 20000;
    static constexpr uint8_t kNoAudio = 255;

    int videoStream = -1;
    int audioStream = -1; // -1: no audio analysed
    std::vector<FrameFeatures> frames;
    // Loudness per 20 ms window of unified time, from audioStartUs, in
    // -dBFS (0 = full scale, 120 = digital silence); kNoAudio = no samples.
    int64_t audioStartUs = 0;
    std::vector<uint8_t> loudness;

    bool isComplete() const;
    // Loudness (-dBFS) of the window containing a unified time, or kNoAudio.
    uint8_t loudnessAt(int64_t timeUs) const;

    bool save(const QString &path, const FileKey &key, QString *error) const;
    static std::unique_ptr<AnalysisData> load(const QString &path, const FileKey &key, QString *error);
    // Next to the index cache: <hash>.analysis
    static QString cachePath(const QString &mediaPath, const QString &cacheDir = {});
};

// Something worth looking at: a run of dark frames (the bottom of a fade or
// a black separator), possibly with silence at the same time.
struct BreakCandidate {
    uint32_t first = 0; // first dark frame
    uint32_t last = 0;  // last dark frame
    bool silent = false; // silence overlaps the dark frames
    bool fade = false;   // the picture faded out into it (brightness falling steadily)
};

// An ad break: a cluster of candidates (spot separators) close together.
struct BreakRegion {
    uint32_t first = 0; // first dark frame of the first candidate (or of the fade before it)
    uint32_t last = 0;  // last dark frame of the last candidate
    uint32_t candidates = 0;
    bool startsWithFade = false; // first comes from a fade before the first separator
};

struct DetectionSettings {
    uint8_t darkFraction = 235;  // a frame is dark when this share (of 255) of it is near black
    uint32_t mergeGapFrames = 3; // dark runs closer than this are one run
    uint8_t silenceDb = 55;      // windows quieter than -55 dBFS are silent
    int64_t minSilenceUs = 200000;
    int64_t silenceToleranceUs = 300000; // how far silence may be from the dark frames
    // Breaks: silent candidates at most this far apart (spots last up to a
    // minute or so; promos a bit more), at least this many of them.
    int64_t maxSpotUs = 100'000'000;
    uint32_t minBreakCandidates = 2;
    // Fades: brightness falling by at least this much over the time before
    // the dark frames, steadily. A break starts at the last fade at most
    // fadeLeadUs before its first separator (on Mediaset: programme fade,
    // 2 s channel ident, first spot, separator).
    int64_t fadeWindowUs = 600000;
    uint8_t fadeDrop = 20;
    int64_t fadeLeadUs = 40'000'000;
};

std::vector<BreakCandidate> findBreakCandidates(const AnalysisData &data, const PacketIndex &index,
                                                const DetectionSettings &settings = {});

std::vector<BreakRegion> findBreaks(const std::vector<BreakCandidate> &candidates, const PacketIndex &index,
                                    int videoStream, const DetectionSettings &settings = {});

// Silent intervals [start, end) in unified time.
std::vector<std::pair<int64_t, int64_t>> findSilences(const AnalysisData &data,
                                                      const DetectionSettings &settings = {});

} // namespace adscutter
