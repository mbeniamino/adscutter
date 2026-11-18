#pragma once

#include "analysis/AnalysisData.h"

#include <functional>
#include <memory>

namespace adscutter {

// Reads the recording once, decoding every frame of the video stream and the
// audio stream (if any) to measure them. Video is decoded in software
// without the deblocking filter: the measurements do not need a perfect
// picture, and this is the fastest way through a whole recording.
//
// progress(fraction) may return false to cancel (nullptr, empty error).
std::unique_ptr<AnalysisData> analyzeRecording(const QString &path, const PacketIndex &index, int videoStream,
                                               int audioStream, const std::function<bool(double)> &progress,
                                               QString *error);

} // namespace adscutter
