#pragma once

// Helpers shared by the media tests.

#include "media/MediaFile.h"

#include <QCryptographicHash>
#include <vector>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

namespace adscutter::test {

// MD5 of the visible picture data of a decoded frame.
inline QByteArray hashFrame(const AVFrame *f)
{
    QCryptographicHash h(QCryptographicHash::Md5);
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(AVPixelFormat(f->format));
    const int planes = av_pix_fmt_count_planes(AVPixelFormat(f->format));
    for (int p = 0; p < planes; ++p) {
        const int lineBytes = av_image_get_linesize(AVPixelFormat(f->format), f->width, p);
        const int rows = (p == 1 || p == 2) ? AV_CEIL_RSHIFT(f->height, desc->log2_chroma_h) : f->height;
        for (int y = 0; y < rows; ++y)
            h.addData(QByteArrayView(f->data[p] + ptrdiff_t(f->linesize[p]) * y, lineBytes));
    }
    return h.result();
}

struct DecodedVideo {
    std::vector<QByteArray> hashes; // in output order
    std::vector<int64_t> pts;       // frame pts in the stream time base
    std::vector<bool> keyframes;
    AVRational timeBase{1, 1};
};

// Decodes every frame of a stream from the start of the file.
inline DecodedVideo decodeAll(const QString &path, int streamIndex)
{
    DecodedVideo out;
    QString error;
    FormatContextPtr fmt = openFormatContext(path, &error);
    if (!fmt)
        return out;
    const AVStream *st = fmt->streams[streamIndex];
    out.timeBase = st->time_base;
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    CodecContextPtr ctx(avcodec_alloc_context3(codec));
    avcodec_parameters_to_context(ctx.get(), st->codecpar);
    ctx->pkt_timebase = st->time_base;
    if (avcodec_open2(ctx.get(), codec, nullptr) < 0)
        return out;
    PacketPtr pkt(av_packet_alloc());
    FramePtr frame(av_frame_alloc());
    auto drain = [&] {
        while (avcodec_receive_frame(ctx.get(), frame.get()) >= 0) {
            out.hashes.push_back(hashFrame(frame.get()));
            out.pts.push_back(frame->best_effort_timestamp);
            out.keyframes.push_back(frame->flags & AV_FRAME_FLAG_KEY);
            av_frame_unref(frame.get());
        }
    };
    while (av_read_frame(fmt.get(), pkt.get()) >= 0) {
        if (pkt->stream_index == streamIndex) {
            avcodec_send_packet(ctx.get(), pkt.get());
            drain();
        }
        av_packet_unref(pkt.get());
    }
    avcodec_send_packet(ctx.get(), nullptr);
    drain();
    return out;
}

} // namespace adscutter::test
