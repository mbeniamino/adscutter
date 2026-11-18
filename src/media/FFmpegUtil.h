#pragma once

// Small RAII helpers and conversions around the FFmpeg C API.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}

#include <QString>
#include <memory>

namespace adscutter {

struct FormatContextDeleter {
    void operator()(AVFormatContext *ctx) const { avformat_close_input(&ctx); }
};
struct CodecContextDeleter {
    void operator()(AVCodecContext *ctx) const { avcodec_free_context(&ctx); }
};
struct PacketDeleter {
    void operator()(AVPacket *pkt) const { av_packet_free(&pkt); }
};
struct FrameDeleter {
    void operator()(AVFrame *frame) const { av_frame_free(&frame); }
};
struct SwsContextDeleter {
    void operator()(SwsContext *ctx) const { sws_freeContext(ctx); }
};

using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;

inline QString avErrorString(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return QString::fromUtf8(buf);
}

} // namespace adscutter
