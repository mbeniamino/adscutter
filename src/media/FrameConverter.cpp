#include "media/FrameConverter.h"

namespace adscutter {

QImage FrameConverter::toImage(const AVFrame *frame)
{
    const auto format = AVPixelFormat(frame->format);
    if (!m_sws || frame->width != m_width || frame->height != m_height || format != m_format) {
        m_sws.reset(sws_getContext(frame->width, frame->height, format, frame->width, frame->height,
                                   AV_PIX_FMT_RGB32, SWS_BILINEAR, nullptr, nullptr, nullptr));
        if (!m_sws)
            return {};
        m_width = frame->width;
        m_height = frame->height;
        m_format = format;
    }

    // AV_PIX_FMT_RGB32 is native-endian 0xAARRGGBB, i.e. QImage::Format_RGB32.
    QImage image(frame->width, frame->height, QImage::Format_RGB32);
    if (image.isNull())
        return {};
    uint8_t *dst[4] = {image.bits(), nullptr, nullptr, nullptr};
    int dstStride[4] = {int(image.bytesPerLine()), 0, 0, 0};
    sws_scale(m_sws.get(), frame->data, frame->linesize, 0, frame->height, dst, dstStride);
    return image;
}

double displayAspect(const AVFrame *frame, AVRational sar)
{
    if (frame->sample_aspect_ratio.num > 0 && frame->sample_aspect_ratio.den > 0)
        sar = frame->sample_aspect_ratio;
    if (sar.num <= 0 || sar.den <= 0)
        sar = AVRational{1, 1};
    return double(frame->width) * sar.num / (double(frame->height) * sar.den);
}

} // namespace adscutter
