#include "media/IndexBuilder.h"
#include "media/Bitstream.h"

#include <cstdlib>

namespace adscutter {

namespace {

// A jump in decode timestamps of a dense (audio/video) stream outside this
// range is treated as a discontinuity. B-frame reordering makes PTS-based
// steps go slightly backwards, hence the negative tolerance.
constexpr int64_t kMaxBackwardStepUs = 500'000;
constexpr int64_t kMaxForwardStepUs = 3'000'000;
// Streams that jump to within this distance of each other are considered
// to have hit the same discontinuity and get the same offset, which keeps
// audio and video in sync across it.
constexpr int64_t kSameDiscontinuityUs = 5'000'000;

struct Discontinuity {
    int64_t newTimeUs; // rescaled timestamp right after the jump
    int64_t offsetUs;  // offset to apply from there on
};

struct StreamState {
    int nalLength = 0;
    bool dense = true; // audio/video: packets at regular short intervals
    int wrapBits = 64;
    int64_t wrapAdd = 0;
    int64_t lastTs = AV_NOPTS_VALUE;       // previous decode timestamp, wrap-corrected
    int64_t lastEndUs = AV_NOPTS_VALUE;    // max unified pts + duration seen so far
    int64_t lastDurationUs = 0;
    int64_t offsetUs = 0;
    size_t discontinuitiesSeen = 0;
};

void startSegment(PacketIndex::Stream &s, int64_t offsetUs)
{
    const auto first = uint32_t(s.packets.size());
    if (!s.segments.empty() && s.segments.back().firstPacket == first)
        s.segments.back().offsetUs = offsetUs;
    else
        s.segments.push_back({first, offsetUs});
}

} // namespace

std::unique_ptr<PacketIndex> buildIndex(AVFormatContext *fmt,
                                        const std::function<bool(double)> &progress,
                                        QString *error)
{
    auto index = std::make_unique<PacketIndex>();
    const unsigned nbStreams = fmt->nb_streams;
    std::vector<StreamState> states(nbStreams);
    std::vector<Discontinuity> discontinuities;

    index->streams().resize(nbStreams);
    for (unsigned i = 0; i < nbStreams; ++i) {
        const AVStream *st = fmt->streams[i];
        PacketIndex::Stream &s = index->streams()[i];
        s.index = int(i);
        s.type = st->codecpar->codec_type;
        s.codecId = st->codecpar->codec_id;
        s.timeBase = st->time_base;

        StreamState &state = states[i];
        state.nalLength = nalLengthSize(s.codecId, st->codecpar->extradata, st->codecpar->extradata_size);
        state.dense = s.type == AVMEDIA_TYPE_VIDEO || s.type == AVMEDIA_TYPE_AUDIO;
        state.wrapBits = st->pts_wrap_bits > 0 && st->pts_wrap_bits < 63 ? st->pts_wrap_bits : 64;
    }

    const int64_t fileSize = fmt->pb ? avio_size(fmt->pb) : -1;
    PacketPtr pkt(av_packet_alloc());
    if (!pkt) {
        if (error)
            *error = QStringLiteral("Out of memory");
        return nullptr;
    }

    uint64_t count = 0;
    for (;;) {
        const int ret = av_read_frame(fmt, pkt.get());
        if (ret == AVERROR(EAGAIN))
            continue;
        if (ret < 0)
            break; // EOF, or a damaged tail: keep what was indexed so far

        if (++count % 512 == 0 && progress && fileSize > 0) {
            const double fraction = double(avio_tell(fmt->pb)) / double(fileSize);
            if (!progress(std::min(1.0, fraction))) {
                if (error)
                    error->clear();
                return nullptr;
            }
        }

        // Streams appearing mid-file (possible with MPEG-TS) were not part
        // of the probed stream list and cannot be selected anyway.
        if (pkt->stream_index < 0 || unsigned(pkt->stream_index) >= nbStreams) {
            av_packet_unref(pkt.get());
            continue;
        }
        PacketIndex::Stream &s = index->streams()[pkt->stream_index];
        StreamState &state = states[pkt->stream_index];

        PacketIndex::Packet entry{};
        entry.pos = pkt->pos;
        entry.size = pkt->size;
        entry.pts = pkt->pts;
        entry.dts = pkt->dts;
        if (pkt->flags & AV_PKT_FLAG_KEY)
            entry.flags |= PacketIndex::Key;
        if (pkt->flags & AV_PKT_FLAG_CORRUPT)
            entry.flags |= PacketIndex::Corrupt;

        if (s.type == AVMEDIA_TYPE_VIDEO) {
            const FrameClass fc = classifyVideoPacket(s.codecId, pkt->data, size_t(pkt->size), state.nalLength);
            if (fc.known) {
                if (fc.randomAccess)
                    entry.flags |= PacketIndex::RandomAccess;
                if (fc.intra)
                    entry.flags |= PacketIndex::Intra;
                if (fc.paramSets)
                    entry.flags |= PacketIndex::ParamSets;
            } else if (pkt->flags & AV_PKT_FLAG_KEY) {
                entry.flags |= PacketIndex::RandomAccess | PacketIndex::Intra;
            }
        } else {
            // Audio frames and subtitles decode independently.
            entry.flags |= PacketIndex::RandomAccess | PacketIndex::Intra;
        }

        // --- Timestamp unification -------------------------------------
        if (s.packets.empty()) {
            // A stream starting after a discontinuity already uses the new clock.
            if (!discontinuities.empty()) {
                state.offsetUs = discontinuities.back().offsetUs;
                state.discontinuitiesSeen = discontinuities.size();
            }
            startSegment(s, state.offsetUs);
        }

        int64_t ts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
        if (ts != AV_NOPTS_VALUE) {
            const bool wraps = state.wrapBits < 64;
            const int64_t full = wraps ? int64_t(1) << state.wrapBits : 0;
            const int64_t half = full / 2;

            ts += state.wrapAdd;
            if (wraps && state.lastTs != AV_NOPTS_VALUE && ts - state.lastTs < -half) {
                state.wrapAdd += full; // e.g. the 33-bit MPEG-TS clock rolled over
                ts += full;
            }
            // PTS and DTS of one packet can be on different sides of a wrap.
            auto unwrap = [&](int64_t v) {
                if (v == AV_NOPTS_VALUE)
                    return v;
                v += state.wrapAdd;
                if (wraps) {
                    if (v - ts < -half)
                        v += full;
                    else if (v - ts > half)
                        v -= full;
                }
                return v;
            };
            entry.pts = unwrap(pkt->pts);
            entry.dts = unwrap(pkt->dts);

            const int64_t tsUs = av_rescale_q(ts, s.timeBase, AV_TIME_BASE_Q);
            if (state.dense && state.lastTs != AV_NOPTS_VALUE) {
                const int64_t stepUs = tsUs - av_rescale_q(state.lastTs, s.timeBase, AV_TIME_BASE_Q);
                if (stepUs < -kMaxBackwardStepUs || stepUs > kMaxForwardStepUs) {
                    // Discontinuity. If another stream already jumped to about
                    // the same time, share its offset; otherwise continue this
                    // stream right where it stopped.
                    bool adopted = false;
                    for (size_t d = state.discontinuitiesSeen; d < discontinuities.size(); ++d) {
                        if (std::llabs(discontinuities[d].newTimeUs - tsUs) < kSameDiscontinuityUs) {
                            state.offsetUs = discontinuities[d].offsetUs;
                            state.discontinuitiesSeen = d + 1;
                            adopted = true;
                            break;
                        }
                    }
                    if (!adopted) {
                        const int64_t continueAt = state.lastEndUs != AV_NOPTS_VALUE
                                                       ? state.lastEndUs
                                                       : tsUs + state.offsetUs;
                        state.offsetUs = continueAt - tsUs;
                        discontinuities.push_back({tsUs, state.offsetUs});
                        state.discontinuitiesSeen = discontinuities.size();
                    }
                    startSegment(s, state.offsetUs);
                }
            } else if (!state.dense && state.discontinuitiesSeen < discontinuities.size()) {
                // Sparse streams (subtitles) cannot detect jumps reliably on
                // their own: follow the clock of the dense streams.
                state.offsetUs = discontinuities.back().offsetUs;
                state.discontinuitiesSeen = discontinuities.size();
                startSegment(s, state.offsetUs);
            }
            state.lastTs = ts;

            const int64_t durationUs = pkt->duration > 0
                                           ? av_rescale_q(pkt->duration, s.timeBase, AV_TIME_BASE_Q)
                                           : state.lastDurationUs;
            state.lastDurationUs = durationUs;
            const int64_t presentedUs = av_rescale_q(entry.pts != AV_NOPTS_VALUE ? entry.pts : ts,
                                                     s.timeBase, AV_TIME_BASE_Q)
                                        + state.offsetUs + durationUs;
            if (state.lastEndUs == AV_NOPTS_VALUE || presentedUs > state.lastEndUs)
                state.lastEndUs = presentedUs;
        }

        entry.segment = uint16_t(s.segments.size() - 1);
        s.packets.push_back(entry);
        av_packet_unref(pkt.get());

        if (s.segments.size() >= 65535) {
            if (error)
                *error = QStringLiteral("Too many timestamp discontinuities");
            return nullptr;
        }
    }

    if (progress)
        progress(1.0);
    index->finalize();
    return index;
}

} // namespace adscutter
