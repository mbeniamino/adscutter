#include "media/AudioDecoder.h"
#include "media/MediaFile.h"

extern "C" {
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cstring>

namespace adscutter {

namespace {
// Decoding starts this many frames before the requested time: AAC and AC-3
// overlap consecutive frames, so the first decoded frame is not clean.
constexpr uint32_t kPreroll = 2;
constexpr int64_t kMaxSyncDistance = int64_t(64) << 20;
} // namespace

AudioDecoder::AudioDecoder() = default;

AudioDecoder::~AudioDecoder()
{
    swr_free(&m_swr);
    av_channel_layout_uninit(&m_swrInLayout);
}

bool AudioDecoder::open(const QString &path, int streamIndex, std::shared_ptr<const PacketIndex> index, int outRate,
                        int outChannels, QString *error)
{
    auto fail = [&](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    m_index = std::move(index);
    m_stream = m_index ? m_index->stream(streamIndex) : nullptr;
    if (!m_stream || m_stream->type != AVMEDIA_TYPE_AUDIO || m_stream->displayOrder.empty())
        return fail(QStringLiteral("No such audio stream"));
    m_outRate = outRate;
    m_outChannels = outChannels;

    m_fmt = openFormatContext(path, error);
    if (!m_fmt)
        return false;
    if (unsigned(streamIndex) >= m_fmt->nb_streams
        || m_fmt->streams[streamIndex]->codecpar->codec_id != m_stream->codecId)
        return fail(QStringLiteral("File does not match its index"));
    m_avStream = m_fmt->streams[streamIndex];
    const char *name = m_fmt->iformat->name;
    m_byteSeek = std::strcmp(name, "mpegts") == 0 || std::strcmp(name, "mpeg") == 0;

    const AVCodec *codec = avcodec_find_decoder(m_avStream->codecpar->codec_id);
    if (!codec)
        return fail(QStringLiteral("No decoder for %1").arg(QString::fromUtf8(avcodec_get_name(m_stream->codecId))));
    m_codec.reset(avcodec_alloc_context3(codec));
    int ret = avcodec_parameters_to_context(m_codec.get(), m_avStream->codecpar);
    if (ret >= 0) {
        m_codec->pkt_timebase = m_avStream->time_base;
        m_codec->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
        ret = avcodec_open2(m_codec.get(), codec, nullptr);
    }
    if (ret < 0)
        return fail(QStringLiteral("Cannot open audio decoder: %1").arg(avErrorString(ret)));
    m_pkt.reset(av_packet_alloc());
    m_frame.reset(av_frame_alloc());
    return seek(m_index->unifiedPts(*m_stream, m_stream->displayOrder.front()), error);
}

bool AudioDecoder::seek(int64_t timeUs, QString *error)
{
    // Frame containing timeUs, minus the preroll.
    auto it = std::partition_point(m_stream->displayOrder.begin(), m_stream->displayOrder.end(),
                                   [&](uint32_t p) { return m_index->unifiedPts(*m_stream, p) <= timeUs; });
    uint32_t pos = uint32_t(it - m_stream->displayOrder.begin());
    pos = pos > 0 ? pos - 1 : 0;
    pos = pos > kPreroll ? pos - kPreroll : 0;
    const uint32_t packet = m_stream->displayOrder[pos];
    const PacketIndex::Packet &entry = m_stream->packets[packet];

    avcodec_flush_buffers(m_codec.get());
    if (m_swr)
        swr_init(m_swr); // drop buffered samples
    int ret = -1;
    if (m_byteSeek && entry.pos >= 0) {
        int64_t target = entry.pos;
        if (packet > 0 && m_stream->packets[packet - 1].pos >= 0)
            target = std::min(target, m_stream->packets[packet - 1].pos);
        ret = av_seek_frame(m_fmt.get(), -1, target, AVSEEK_FLAG_BYTE);
    } else if (entry.pts != AV_NOPTS_VALUE) {
        ret = av_seek_frame(m_fmt.get(), m_avStream->index, entry.pts, AVSEEK_FLAG_BACKWARD);
    }
    if (ret < 0)
        ret = av_seek_frame(m_fmt.get(), -1, 0, AVSEEK_FLAG_BYTE);
    if (ret < 0) {
        if (error)
            *error = QStringLiteral("Audio seek failed: %1").arg(avErrorString(ret));
        return false;
    }
    m_nextPacket = packet;
    m_synced = false;
    m_inputDone = false;
    m_trimBeforeUs = timeUs;
    return true;
}

bool AudioDecoder::feedPacket(QString *error)
{
    for (;;) {
        if (m_nextPacket >= m_stream->packets.size()) {
            avcodec_send_packet(m_codec.get(), nullptr);
            m_inputDone = true;
            return true;
        }
        const int ret = av_read_frame(m_fmt.get(), m_pkt.get());
        if (ret == AVERROR(EAGAIN))
            continue;
        if (ret < 0) {
            avcodec_send_packet(m_codec.get(), nullptr);
            m_inputDone = true;
            return true;
        }
        if (m_pkt->stream_index != m_avStream->index) {
            av_packet_unref(m_pkt.get());
            continue;
        }
        // Several audio frames of one PES share its byte position (and often
        // their size), so the timestamp must match too (modulo the clock wrap).
        const PacketIndex::Packet &want = m_stream->packets[m_nextPacket];
        bool samePts = true;
        if (m_pkt->pts != AV_NOPTS_VALUE && want.pts != AV_NOPTS_VALUE) {
            const int bits = m_avStream->pts_wrap_bits;
            const int64_t diff = m_pkt->pts - want.pts;
            samePts = bits > 0 && bits < 63 ? (diff & ((int64_t(1) << bits) - 1)) == 0 : diff == 0;
        }
        const bool matches = m_pkt->size == want.size && (want.pos < 0 || m_pkt->pos == want.pos) && samePts;
        if (!m_synced && !matches) {
            const bool passed = want.pos >= 0 && m_pkt->pos > want.pos + kMaxSyncDistance;
            av_packet_unref(m_pkt.get());
            if (passed) {
                if (error)
                    *error = QStringLiteral("Cannot find the audio after seeking");
                return false;
            }
            continue;
        }
        m_synced = true; // after syncing, take packets in order (a mismatch is tolerated for playback)
        m_pkt->opaque = reinterpret_cast<void *>(uintptr_t(m_nextPacket) + 1);
        ++m_nextPacket;
        avcodec_send_packet(m_codec.get(), m_pkt.get()); // broken packets are skipped by the decoder
        av_packet_unref(m_pkt.get());
        return true;
    }
}

bool AudioDecoder::convert(const AVFrame *frame, std::vector<float> &samples, int64_t *startUs)
{
    // (Re)configure the resampler when the input format changes.
    if (!m_swr || frame->sample_rate != m_swrInRate || frame->format != m_swrInFormat
        || av_channel_layout_compare(&frame->ch_layout, &m_swrInLayout) != 0) {
        swr_free(&m_swr);
        AVChannelLayout outLayout;
        av_channel_layout_default(&outLayout, m_outChannels);
        if (swr_alloc_set_opts2(&m_swr, &outLayout, AV_SAMPLE_FMT_FLT, m_outRate, &frame->ch_layout,
                                AVSampleFormat(frame->format), frame->sample_rate, 0, nullptr) < 0
            || swr_init(m_swr) < 0) {
            av_channel_layout_uninit(&outLayout);
            swr_free(&m_swr);
            return false;
        }
        av_channel_layout_uninit(&outLayout);
        av_channel_layout_uninit(&m_swrInLayout);
        av_channel_layout_copy(&m_swrInLayout, &frame->ch_layout);
        m_swrInRate = frame->sample_rate;
        m_swrInFormat = frame->format;
    }

    const auto packet = uintptr_t(frame->opaque);
    int64_t t = packet >= 1 && packet <= m_stream->packets.size() ? m_index->unifiedPts(*m_stream, uint32_t(packet - 1))
                                                                    : AV_NOPTS_VALUE;
    const int maxOut = swr_get_out_samples(m_swr, frame->nb_samples);
    samples.resize(size_t(maxOut) * size_t(m_outChannels));
    uint8_t *out[1] = {reinterpret_cast<uint8_t *>(samples.data())};
    const int got = swr_convert(m_swr, out, maxOut, const_cast<const uint8_t **>(frame->extended_data), frame->nb_samples);
    if (got < 0)
        return false;
    samples.resize(size_t(got) * size_t(m_outChannels));
    if (t == AV_NOPTS_VALUE)
        t = m_trimBeforeUs;
    *startUs = t;

    // Trim what precedes the seek target.
    if (m_trimBeforeUs != INT64_MIN) {
        const int64_t skip = av_rescale(m_trimBeforeUs - t, m_outRate, AV_TIME_BASE);
        if (skip > 0) {
            const size_t drop = std::min(samples.size(), size_t(skip) * size_t(m_outChannels));
            samples.erase(samples.begin(), samples.begin() + ptrdiff_t(drop));
            *startUs = m_trimBeforeUs;
        }
        if (!samples.empty())
            m_trimBeforeUs = INT64_MIN;
    }
    return true;
}

bool AudioDecoder::read(std::vector<float> &samples, int64_t *startUs, QString *error)
{
    samples.clear();
    for (;;) {
        const int ret = avcodec_receive_frame(m_codec.get(), m_frame.get());
        if (ret >= 0) {
            const bool ok = convert(m_frame.get(), samples, startUs);
            av_frame_unref(m_frame.get());
            if (!ok) {
                if (error)
                    *error = QStringLiteral("Cannot convert audio");
                return false;
            }
            if (!samples.empty())
                return true;
            continue; // fully trimmed
        }
        if (ret == AVERROR_EOF)
            return false;
        if (ret != AVERROR(EAGAIN)) {
            if (error)
                *error = QStringLiteral("Audio decoding failed: %1").arg(avErrorString(ret));
            return false;
        }
        if (m_inputDone)
            return false;
        if (!feedPacket(error))
            return false;
    }
}

} // namespace adscutter
