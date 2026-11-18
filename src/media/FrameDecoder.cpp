#include "media/FrameDecoder.h"
#include "media/MediaFile.h"

namespace adscutter {

bool FrameDecoder::open(MediaFile &file, int streamIndex, QString *error)
{
    m_codec.reset();
    m_sws.reset();
    m_fmt = file.formatContext();
    if (!m_fmt || streamIndex < 0 || unsigned(streamIndex) >= m_fmt->nb_streams) {
        if (error)
            *error = QStringLiteral("Invalid video stream");
        return false;
    }
    m_stream = m_fmt->streams[streamIndex];

    const AVCodec *codec = avcodec_find_decoder(m_stream->codecpar->codec_id);
    if (!codec) {
        if (error)
            *error = QStringLiteral("No decoder for %1")
                         .arg(QString::fromUtf8(avcodec_get_name(m_stream->codecpar->codec_id)));
        return false;
    }

    CodecContextPtr ctx(avcodec_alloc_context3(codec));
    if (!ctx) {
        if (error)
            *error = QStringLiteral("Out of memory");
        return false;
    }
    int ret = avcodec_parameters_to_context(ctx.get(), m_stream->codecpar);
    if (ret >= 0) {
        ctx->pkt_timebase = m_stream->time_base;
        ctx->thread_count = 0; // auto
        ret = avcodec_open2(ctx.get(), codec, nullptr);
    }
    if (ret < 0) {
        if (error)
            *error = QStringLiteral("Cannot open decoder: %1").arg(avErrorString(ret));
        return false;
    }
    m_codec = std::move(ctx);
    return true;
}

std::optional<DecodedFrame> FrameDecoder::decodeNextFrame(QString *error)
{
    if (!m_codec) {
        if (error)
            *error = QStringLiteral("Decoder not open");
        return std::nullopt;
    }

    PacketPtr pkt(av_packet_alloc());
    FramePtr frame(av_frame_alloc());
    bool flushing = false;

    for (;;) {
        int ret = avcodec_receive_frame(m_codec.get(), frame.get());
        if (ret >= 0) {
            auto result = convert(frame.get());
            av_frame_unref(frame.get());
            if (result)
                return result;
            if (error)
                *error = QStringLiteral("Cannot convert decoded frame");
            return std::nullopt;
        }
        if (ret == AVERROR_EOF) {
            if (error)
                *error = QStringLiteral("End of stream reached without a decodable frame");
            return std::nullopt;
        }
        if (ret != AVERROR(EAGAIN)) {
            if (error)
                *error = QStringLiteral("Decoding failed: %1").arg(avErrorString(ret));
            return std::nullopt;
        }
        if (flushing)
            continue;

        // Decoder needs more input.
        ret = av_read_frame(m_fmt, pkt.get());
        if (ret == AVERROR_EOF) {
            avcodec_send_packet(m_codec.get(), nullptr);
            flushing = true;
            continue;
        }
        if (ret < 0) {
            if (error)
                *error = QStringLiteral("Read error: %1").arg(avErrorString(ret));
            return std::nullopt;
        }
        if (pkt->stream_index == m_stream->index) {
            // Corrupt packets at the start of a TV recording are normal;
            // the decoder skips them until a usable entry point.
            ret = avcodec_send_packet(m_codec.get(), pkt.get());
            if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_INVALIDDATA) {
                av_packet_unref(pkt.get());
                if (error)
                    *error = QStringLiteral("Decoding failed: %1").arg(avErrorString(ret));
                return std::nullopt;
            }
        }
        av_packet_unref(pkt.get());
    }
}

std::optional<DecodedFrame> FrameDecoder::convert(const AVFrame *frame)
{
    const auto format = AVPixelFormat(frame->format);
    if (!m_sws || frame->width != m_swsWidth || frame->height != m_swsHeight
        || format != m_swsFormat) {
        m_sws.reset(sws_getContext(frame->width, frame->height, format,
                                   frame->width, frame->height, AV_PIX_FMT_RGB32,
                                   SWS_BILINEAR, nullptr, nullptr, nullptr));
        if (!m_sws)
            return std::nullopt;
        m_swsWidth = frame->width;
        m_swsHeight = frame->height;
        m_swsFormat = format;
    }

    // AV_PIX_FMT_RGB32 is native-endian 0xAARRGGBB, which is exactly
    // QImage::Format_RGB32.
    QImage image(frame->width, frame->height, QImage::Format_RGB32);
    if (image.isNull())
        return std::nullopt;
    uint8_t *dst[4] = {image.bits(), nullptr, nullptr, nullptr};
    int dstStride[4] = {int(image.bytesPerLine()), 0, 0, 0};
    sws_scale(m_sws.get(), frame->data, frame->linesize, 0, frame->height, dst, dstStride);

    DecodedFrame out;
    out.image = std::move(image);
    out.pts = frame->best_effort_timestamp;
    out.keyframe = frame->flags & AV_FRAME_FLAG_KEY;
    out.interlaced = frame->flags & AV_FRAME_FLAG_INTERLACED;

    AVRational sar = av_guess_sample_aspect_ratio(m_fmt, m_stream, const_cast<AVFrame *>(frame));
    if (sar.num <= 0 || sar.den <= 0)
        sar = AVRational{1, 1};
    out.displayAspect = double(frame->width) * sar.num / (double(frame->height) * sar.den);
    return out;
}

} // namespace adscutter
