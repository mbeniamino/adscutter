#pragma once

#include "media/FFmpegUtil.h"
#include "media/FrameConverter.h"

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
// The decoder opens its own demuxer on the file, independent from the one
// used by the indexer, so both can run at the same time.
//
// Sequential decoding from the start of the file, used to show a preview
// before the index is ready. Random access is RandomAccessDecoder's job.
class FrameDecoder {
public:
    FrameDecoder() = default;
    FrameDecoder(const FrameDecoder &) = delete;
    FrameDecoder &operator=(const FrameDecoder &) = delete;

    bool open(const MediaFile &file, int streamIndex, QString *error);

    // Reads from the current demuxer position until a frame is produced.
    std::optional<DecodedFrame> decodeNextFrame(QString *error);

private:
    std::optional<DecodedFrame> convert(const AVFrame *frame);

    FormatContextPtr m_fmt;
    AVStream *m_stream = nullptr;
    CodecContextPtr m_codec;
    FrameConverter m_converter;
};

} // namespace adscutter
