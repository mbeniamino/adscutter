#pragma once

#include "media/FFmpegUtil.h"

#include <QImage>

namespace adscutter {

// Converts decoded frames to RGB32 QImages at their coded resolution.
class FrameConverter {
public:
    QImage toImage(const AVFrame *frame);

private:
    SwsContextPtr m_sws;
    int m_width = 0, m_height = 0;
    AVPixelFormat m_format = AV_PIX_FMT_NONE;
};

// Width / height ratio a frame should be displayed with.
double displayAspect(const AVFrame *frame, AVRational sampleAspect);

} // namespace adscutter
