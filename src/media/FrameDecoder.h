#pragma once

#include "media/FFmpegUtil.h"

#include <QImage>
#include <optional>

namespace adscutter {

class MediaFile;

struct DecodedFrame {
    QImage image;          // RGB32, coded resolution (not aspect corrected)
    int64_t pts = AV_NOPTS_VALUE; // in stream time base
    bool keyframe = false;
    bool interlaced = false;
    double displayAspect = 0.0; // width / height as it should be shown
};

// Decodes frames of one video stream of a MediaFile.
//
// Milestone 1 only supports decoding the first displayable frame; seeking
// and the GOP cache arrive with frame-accurate navigation (milestone 3).
class FrameDecoder {
public:
    FrameDecoder() = default;
    FrameDecoder(const FrameDecoder &) = delete;
    FrameDecoder &operator=(const FrameDecoder &) = delete;

    bool open(MediaFile &file, int streamIndex, QString *error);

    // Reads from the current demuxer position until a frame is produced.
    std::optional<DecodedFrame> decodeNextFrame(QString *error);

private:
    std::optional<DecodedFrame> convert(const AVFrame *frame);

    AVFormatContext *m_fmt = nullptr; // owned by MediaFile
    AVStream *m_stream = nullptr;
    CodecContextPtr m_codec;
    SwsContextPtr m_sws;
    int m_swsWidth = 0, m_swsHeight = 0;
    AVPixelFormat m_swsFormat = AV_PIX_FMT_NONE;
};

} // namespace adscutter
