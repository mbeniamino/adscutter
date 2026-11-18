#include "analysis/Analyzer.h"
#include "media/MediaFile.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <array>
#include <cmath>

namespace adscutter {

namespace {

// Measurements are taken on a sparse grid: cheap enough to keep up with a
// decoder producing thousands of frames per second.
constexpr int kGridX = 160;
constexpr int kGridY = 90;
constexpr int kDarkLuma = 40; // near black (video black is 16)

class VideoMeter {
public:
    void measure(const AVFrame *frame, FrameFeatures &out)
    {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(AVPixelFormat(frame->format));
        const int depth = desc ? desc->comp[0].depth : 8;
        const bool wide = depth > 8;
        std::array<uint8_t, kGridX * kGridY> grid;
        uint32_t sum = 0, dark = 0, diff = 0;
        for (int gy = 0; gy < kGridY; ++gy) {
            const int y = (2 * gy + 1) * frame->height / (2 * kGridY);
            const uint8_t *row = frame->data[0] + ptrdiff_t(frame->linesize[0]) * y;
            for (int gx = 0; gx < kGridX; ++gx) {
                const int x = (2 * gx + 1) * frame->width / (2 * kGridX);
                const int v = wide ? reinterpret_cast<const uint16_t *>(row)[x] >> (depth - 8) : row[x];
                const auto luma = uint8_t(std::min(v, 255));
                grid[size_t(gy * kGridX + gx)] = luma;
                sum += luma;
                dark += luma < kDarkLuma;
                if (m_havePrevious)
                    diff += uint32_t(std::abs(int(luma) - int(m_previous[size_t(gy * kGridX + gx)])));
            }
        }
        constexpr uint32_t n = kGridX * kGridY;
        out.mean = uint8_t(sum / n);
        out.dark = uint8_t(dark * 255 / n);
        out.scene = uint8_t(std::min<uint32_t>(255, diff / n));
        out.valid = 1;
        m_previous = grid;
        m_havePrevious = true;
    }

private:
    std::array<uint8_t, kGridX * kGridY> m_previous{};
    bool m_havePrevious = false;
};

// Loudness per 20 ms window: mean square of the mono downmix.
class AudioMeter {
public:
    explicit AudioMeter(int64_t startUs)
        : m_startUs(startUs)
    {
    }
    ~AudioMeter() { swr_free(&m_swr); }

    void measure(const AVFrame *frame, int64_t timeUs)
    {
        if (!m_swr || frame->sample_rate != m_rate || frame->format != m_format) {
            swr_free(&m_swr);
            AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
            if (swr_alloc_set_opts2(&m_swr, &mono, AV_SAMPLE_FMT_FLT, frame->sample_rate, &frame->ch_layout,
                                    AVSampleFormat(frame->format), frame->sample_rate, 0, nullptr) < 0
                || swr_init(m_swr) < 0) {
                swr_free(&m_swr);
                return;
            }
            m_rate = frame->sample_rate;
            m_format = frame->format;
        }
        m_samples.resize(size_t(frame->nb_samples) + 32);
        uint8_t *out[1] = {reinterpret_cast<uint8_t *>(m_samples.data())};
        const int n = swr_convert(m_swr, out, int(m_samples.size()), const_cast<const uint8_t **>(frame->extended_data),
                                  frame->nb_samples);
        for (int i = 0; i < n; ++i) {
            const int64_t t = timeUs + int64_t(i) * AV_TIME_BASE / m_rate - m_startUs;
            if (t < 0)
                continue;
            const auto w = size_t(t / AnalysisData::kAudioWindowUs);
            if (w >= m_sumSquares.size()) {
                m_sumSquares.resize(w + 1, 0.0);
                m_count.resize(w + 1, 0);
            }
            m_sumSquares[w] += double(m_samples[size_t(i)]) * m_samples[size_t(i)];
            ++m_count[w];
        }
    }

    std::vector<uint8_t> result() const
    {
        std::vector<uint8_t> out(m_sumSquares.size(), AnalysisData::kNoAudio);
        for (size_t w = 0; w < out.size(); ++w) {
            if (m_count[w] == 0)
                continue;
            const double meanSquare = m_sumSquares[w] / m_count[w];
            const double db = meanSquare > 1e-12 ? -10.0 * std::log10(meanSquare) : 120.0;
            out[w] = uint8_t(std::clamp(db, 0.0, 120.0));
        }
        return out;
    }

private:
    int64_t m_startUs;
    SwrContext *m_swr = nullptr;
    int m_rate = 0;
    int m_format = -1;
    std::vector<float> m_samples;
    std::vector<double> m_sumSquares;
    std::vector<uint32_t> m_count;
};

CodecContextPtr openDecoder(const AVStream *st, bool video, QString *error)
{
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    CodecContextPtr ctx(codec ? avcodec_alloc_context3(codec) : nullptr);
    int ret = ctx ? avcodec_parameters_to_context(ctx.get(), st->codecpar) : AVERROR(ENOMEM);
    if (ret >= 0) {
        ctx->pkt_timebase = st->time_base;
        ctx->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
        if (video) {
            ctx->thread_count = 0;
            ctx->skip_loop_filter = AVDISCARD_ALL; // faster; not needed for measuring
        }
        ret = avcodec_open2(ctx.get(), codec, nullptr);
    }
    if (ret < 0) {
        if (error)
            *error = QStringLiteral("Cannot open %1 decoder: %2")
                         .arg(QString::fromUtf8(avcodec_get_name(st->codecpar->codec_id)), avErrorString(ret));
        return nullptr;
    }
    return ctx;
}

} // namespace

std::unique_ptr<AnalysisData> analyzeRecording(const QString &path, const PacketIndex &index, int videoStream,
                                               int audioStream, const std::function<bool(double)> &progress,
                                               QString *error)
{
    auto fail = [&](const QString &why) -> std::unique_ptr<AnalysisData> {
        if (error)
            *error = why;
        return nullptr;
    };
    const PacketIndex::Stream *video = index.stream(videoStream);
    if (!video || video->displayOrder.empty())
        return fail(QStringLiteral("No video to analyse"));
    const PacketIndex::Stream *audio = audioStream >= 0 ? index.stream(audioStream) : nullptr;
    if (audio && (audio->type != AVMEDIA_TYPE_AUDIO || audio->displayOrder.empty()))
        audio = nullptr;

    FormatContextPtr fmt = openFormatContext(path, error);
    if (!fmt)
        return nullptr;
    if (fmt->nb_streams != index.streams().size())
        return fail(QStringLiteral("The file does not match its index; rebuild the index"));
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (int(i) != videoStream && (!audio || int(i) != audioStream))
            fmt->streams[i]->discard = AVDISCARD_ALL; // the demuxer can skip them
    }

    CodecContextPtr vdec = openDecoder(fmt->streams[videoStream], true, error);
    if (!vdec)
        return nullptr;
    CodecContextPtr adec;
    if (audio && !(adec = openDecoder(fmt->streams[audioStream], false, error)))
        return nullptr;

    auto data = std::make_unique<AnalysisData>();
    data->videoStream = videoStream;
    data->audioStream = audio ? audioStream : -1;
    data->frames.resize(video->displayOrder.size());
    data->audioStartUs = audio ? index.unifiedPts(*audio, audio->displayOrder.front()) : 0;
    VideoMeter videoMeter;
    AudioMeter audioMeter(data->audioStartUs);

    PacketPtr pkt(av_packet_alloc());
    FramePtr frame(av_frame_alloc());
    auto drainVideo = [&] {
        while (avcodec_receive_frame(vdec.get(), frame.get()) >= 0) {
            const auto packet = uintptr_t(frame->opaque);
            const int32_t pos = packet >= 1 && packet <= video->packets.size() ? video->displayPosOf[packet - 1] : -1;
            if (pos >= 0)
                videoMeter.measure(frame.get(), data->frames[size_t(pos)]);
            av_frame_unref(frame.get());
        }
    };
    auto drainAudio = [&] {
        while (avcodec_receive_frame(adec.get(), frame.get()) >= 0) {
            const auto packet = uintptr_t(frame->opaque);
            if (packet >= 1 && packet <= audio->packets.size()) {
                const int64_t t = index.unifiedPts(*audio, uint32_t(packet - 1));
                if (t != AV_NOPTS_VALUE)
                    audioMeter.measure(frame.get(), t);
            }
            av_frame_unref(frame.get());
        }
    };

    std::vector<uint32_t> counters(fmt->nb_streams, 0);
    const int64_t fileSize = avio_size(fmt->pb);
    uint64_t count = 0;
    for (;;) {
        const int ret = av_read_frame(fmt.get(), pkt.get());
        if (ret == AVERROR(EAGAIN))
            continue;
        if (ret < 0)
            break;
        const int s = pkt->stream_index;
        const PacketIndex::Stream *stream = s == videoStream ? video : (audio && s == audioStream ? audio : nullptr);
        if (!stream) {
            av_packet_unref(pkt.get());
            continue;
        }
        const uint32_t i = counters[size_t(s)]++;
        if (i >= stream->packets.size() || stream->packets[i].size != pkt->size) {
            av_packet_unref(pkt.get());
            return fail(QStringLiteral("The file does not match its index; rebuild the index"));
        }
        pkt->opaque = reinterpret_cast<void *>(uintptr_t(i) + 1);
        if (stream == video) {
            avcodec_send_packet(vdec.get(), pkt.get());
            drainVideo();
        } else {
            avcodec_send_packet(adec.get(), pkt.get());
            drainAudio();
        }
        av_packet_unref(pkt.get());

        if (++count % 256 == 0 && progress && fileSize > 0
            && !progress(std::min(1.0, double(avio_tell(fmt->pb)) / double(fileSize)))) {
            if (error)
                error->clear();
            return nullptr;
        }
    }
    avcodec_send_packet(vdec.get(), nullptr);
    drainVideo();
    if (adec) {
        avcodec_send_packet(adec.get(), nullptr);
        drainAudio();
    }
    data->loudness = audioMeter.result();
    if (progress)
        progress(1.0);
    return data;
}

} // namespace adscutter
