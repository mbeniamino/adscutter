#include "media/RandomAccessDecoder.h"
#include "media/MediaFile.h"

#include <cstring>

namespace adscutter {

namespace {

// Continue the current run instead of seeking when the requested frame is
// at most this many frames ahead of the last decoded one.
constexpr int64_t kMaxForwardDecode = 250;
// Give up looking for the expected packet after a seek past this much data.
constexpr int64_t kMaxSyncDistance = int64_t(64) << 20;

size_t frameBytes(const AVFrame *f)
{
    size_t bytes = 0;
    for (const AVBufferRef *buf : f->buf) {
        if (buf)
            bytes += buf->size;
    }
    return bytes;
}

} // namespace

RandomAccessDecoder::RandomAccessDecoder() = default;
RandomAccessDecoder::~RandomAccessDecoder() = default;

bool RandomAccessDecoder::open(const QString &path, int streamIndex,
                               std::shared_ptr<const PacketIndex> index, QString *error)
{
    auto fail = [&](const QString &why) {
        if (error)
            *error = why;
        return false;
    };

    m_index = std::move(index);
    m_stream = m_index ? m_index->stream(streamIndex) : nullptr;
    if (!m_stream || m_stream->type != AVMEDIA_TYPE_VIDEO || m_stream->displayOrder.empty())
        return fail(QStringLiteral("No decodable video stream in the index"));

    m_fmt = openFormatContext(path, error);
    if (!m_fmt)
        return false;
    if (unsigned(streamIndex) >= m_fmt->nb_streams
        || m_fmt->streams[streamIndex]->codecpar->codec_id != m_stream->codecId)
        return fail(QStringLiteral("File does not match its index"));
    m_avStream = m_fmt->streams[streamIndex];
    m_sampleAspect = av_guess_sample_aspect_ratio(m_fmt.get(), m_avStream, nullptr);

    // Byte positions are reliable seek targets in MPEG transport/program
    // streams; container formats with their own index seek by timestamp.
    const char *name = m_fmt->iformat->name;
    m_byteSeek = std::strcmp(name, "mpegts") == 0 || std::strcmp(name, "mpeg") == 0;

    const AVCodec *codec = avcodec_find_decoder(m_avStream->codecpar->codec_id);
    if (!codec)
        return fail(QStringLiteral("No decoder for %1")
                        .arg(QString::fromUtf8(avcodec_get_name(m_avStream->codecpar->codec_id))));
    m_codec.reset(avcodec_alloc_context3(codec));
    if (!m_codec)
        return fail(QStringLiteral("Out of memory"));
    int ret = avcodec_parameters_to_context(m_codec.get(), m_avStream->codecpar);
    if (ret >= 0) {
        m_codec->pkt_timebase = m_avStream->time_base;
        m_codec->thread_count = 0;
        // Frames carry the index of the packet they were decoded from.
        m_codec->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
        ret = avcodec_open2(m_codec.get(), codec, nullptr);
    }
    if (ret < 0)
        return fail(QStringLiteral("Cannot open decoder: %1").arg(avErrorString(ret)));

    m_pkt.reset(av_packet_alloc());
    m_frame.reset(av_frame_alloc());
    if (!m_pkt || !m_frame)
        return fail(QStringLiteral("Out of memory"));
    m_runValid = false;
    return true;
}

void RandomAccessDecoder::setCacheBudget(size_t bytes)
{
    m_cacheBudget = bytes;
    evict();
}

void RandomAccessDecoder::store(int32_t pos, FramePtr frame)
{
    auto it = m_cache.find(uint32_t(pos));
    if (it != m_cache.end()) {
        m_lru.splice(m_lru.begin(), m_lru, it->second.lru);
        return; // already cached (identical content)
    }
    const size_t bytes = frameBytes(frame.get());
    m_lru.push_front(uint32_t(pos));
    m_cache.emplace(uint32_t(pos), Entry{std::move(frame), bytes, m_lru.begin()});
    m_cacheBytes += bytes;
    evict();
}

void RandomAccessDecoder::evict()
{
    // Always keep at least the most recent frame.
    while (m_cacheBytes > m_cacheBudget && m_lru.size() > 1) {
        const uint32_t pos = m_lru.back();
        m_lru.pop_back();
        auto it = m_cache.find(pos);
        m_cacheBytes -= it->second.bytes;
        m_cache.erase(it);
    }
}

bool RandomAccessDecoder::startRun(uint32_t keyPos, QString *error)
{
    ++m_stats.seeks;
    const uint32_t keyPacket = m_stream->displayOrder[keyPos];
    const PacketIndex::Packet &key = m_stream->packets[keyPacket];

    avcodec_flush_buffers(m_codec.get());
    m_runValid = false;

    int ret = -1;
    if (m_byteSeek && key.pos >= 0) {
        // Start a little before the cut point so that the demuxer and the
        // parser are in sync when its packet arrives.
        int64_t target = 0;
        if (keyPacket > 0 && m_stream->packets[keyPacket - 1].pos >= 0)
            target = std::min(m_stream->packets[keyPacket - 1].pos, key.pos);
        ret = av_seek_frame(m_fmt.get(), -1, target, AVSEEK_FLAG_BYTE);
    } else if (key.pts != AV_NOPTS_VALUE) {
        ret = av_seek_frame(m_fmt.get(), m_avStream->index, key.pts, AVSEEK_FLAG_BACKWARD);
    }
    if (ret < 0) {
        // Last resort: read from the start of the file until the packet shows up.
        ret = av_seek_frame(m_fmt.get(), -1, 0, AVSEEK_FLAG_BYTE);
        if (ret < 0)
            ret = avformat_seek_file(m_fmt.get(), -1, INT64_MIN, INT64_MIN, INT64_MIN, 0);
    }
    if (ret < 0) {
        if (error)
            *error = QStringLiteral("Seek failed: %1").arg(avErrorString(ret));
        return false;
    }

    m_nextPacket = keyPacket;
    m_synced = false;
    m_inputDone = false;
    m_runFirstPos = keyPos;
    m_lastOutput = int64_t(keyPos) - 1;
    m_runValid = true;
    return true;
}

bool RandomAccessDecoder::feedPacket(QString *error)
{
    for (;;) {
        if (m_nextPacket >= m_stream->packets.size()) {
            // Everything indexed was sent: drain the decoder.
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

        const PacketIndex::Packet &want = m_stream->packets[m_nextPacket];
        const bool matches = m_pkt->size == want.size && (want.pos < 0 || m_pkt->pos == want.pos);
        if (!m_synced) {
            if (!matches) {
                const bool passed = want.pos >= 0 && m_pkt->pos > want.pos + kMaxSyncDistance;
                av_packet_unref(m_pkt.get());
                if (passed) {
                    m_runValid = false;
                    if (error)
                        *error = QStringLiteral("Cannot find the frame after seeking");
                    return false;
                }
                continue; // still before the cut point
            }
            m_synced = true;
        } else if (!matches) {
            av_packet_unref(m_pkt.get());
            m_runValid = false;
            if (error)
                *error = QStringLiteral("Packets differ from the index; try rebuilding it");
            return false;
        }

        m_pkt->opaque = reinterpret_cast<void *>(uintptr_t(m_nextPacket) + 1);
        ++m_nextPacket;
        const int sent = avcodec_send_packet(m_codec.get(), m_pkt.get());
        av_packet_unref(m_pkt.get());
        if (sent < 0 && sent != AVERROR_INVALIDDATA && sent != AVERROR(EAGAIN)) {
            m_runValid = false;
            if (error)
                *error = QStringLiteral("Decoding failed: %1").arg(avErrorString(sent));
            return false;
        }
        return true;
    }
}

FramePtr RandomAccessDecoder::frameAt(uint32_t pos, QString *error)
{
    if (!m_codec || pos >= frameCount()) {
        if (error)
            *error = QStringLiteral("Frame out of range");
        return nullptr;
    }

    if (auto it = m_cache.find(pos); it != m_cache.end()) {
        ++m_stats.cacheHits;
        m_lru.splice(m_lru.begin(), m_lru, it->second.lru);
        return FramePtr(av_frame_clone(it->second.frame.get()));
    }

    const uint32_t keyPos = PacketIndex::keyframeAtOrBefore(*m_stream, pos);
    // A run stays usable while the decoder drains its last frames at the end
    // of the file (frame threading keeps several in flight); it ends when the
    // decoder reports EOF, which invalidates it.
    const bool canContinue = m_runValid && int64_t(pos) > m_lastOutput
                             && int64_t(pos) - m_lastOutput <= kMaxForwardDecode
                             && m_runFirstPos <= int64_t(keyPos);
    if (!canContinue && !startRun(keyPos, error))
        return nullptr;

    for (;;) {
        const int ret = avcodec_receive_frame(m_codec.get(), m_frame.get());
        if (ret >= 0) {
            const auto packet = uintptr_t(m_frame->opaque);
            const int32_t framePos = (packet >= 1 && packet <= m_stream->packets.size())
                                         ? m_stream->displayPosOf[packet - 1]
                                         : -1;
            // Frames shown before the cut point the run started from are
            // leading pictures of an open GOP: they reference frames that
            // were never decoded.
            if (framePos < m_runFirstPos) {
                av_frame_unref(m_frame.get());
                continue;
            }
            ++m_stats.decodedFrames;
            m_lastOutput = std::max<int64_t>(m_lastOutput, framePos);
            FramePtr frame(av_frame_alloc());
            av_frame_move_ref(frame.get(), m_frame.get());

            if (framePos == int32_t(pos)) {
                FramePtr result(av_frame_clone(frame.get()));
                store(framePos, std::move(frame));
                return result;
            }
            store(framePos, std::move(frame));
            if (framePos > int32_t(pos)) {
                // The decoder skipped the frame (e.g. it was corrupt).
                if (error)
                    *error = QStringLiteral("Frame %1 could not be decoded").arg(pos);
                return nullptr;
            }
            continue;
        }

        if (ret == AVERROR_EOF) {
            m_runValid = false;
            if (error)
                *error = QStringLiteral("Frame %1 could not be decoded").arg(pos);
            return nullptr;
        }
        if (ret != AVERROR(EAGAIN)) {
            m_runValid = false;
            if (error)
                *error = QStringLiteral("Decoding failed: %1").arg(avErrorString(ret));
            return nullptr;
        }
        if (m_inputDone) {
            // Drained without EOF should not happen; avoid spinning.
            m_runValid = false;
            if (error)
                *error = QStringLiteral("Frame %1 could not be decoded").arg(pos);
            return nullptr;
        }
        if (!feedPacket(error))
            return nullptr;
    }
}

} // namespace adscutter
