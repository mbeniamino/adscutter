#pragma once

#include "media/FFmpegUtil.h"

namespace adscutter {

// Display-only deinterlacing of a single frame (no neighbouring frames
// needed, so it works with random access).
//
// Keeps the field displayed first and rebuilds the other one with edge
// directed line interpolation (ELA). Supports planar YUV formats of up to
// 16 bits per sample; returns nullptr for anything else.
FramePtr deinterlaceFrame(const AVFrame *src);

} // namespace adscutter
