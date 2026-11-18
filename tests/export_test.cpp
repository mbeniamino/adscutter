// Exports segments of a file to Matroska and verifies the result.
//
// usage: export_test FILE SEGMENTS [STREAMS]
//   SEGMENTS: comma separated in-out display positions, e.g. "17-39,62-149"
//             (ins are snapped back to cut points, like the UI does)
//   STREAMS:  comma separated stream indices; default: every exportable one
//
// Checks:
//  - every selected stream is present, with codec and language preserved;
//  - the decoded output video is exactly the source frames in..out of each
//    segment (plus the reported extra reference frames), decoded without
//    any error message;
//  - video timestamps advance one frame at a time and every frame of a
//    segment keeps the same source -> output offset;
//  - every output audio packet is a source packet (same bytes) taken from
//    the same offset as its segment's video, i.e. A/V sync is exact, and
//    audio covers each segment within half an audio frame.

#include "TestUtil.h"
#include "media/Exporter.h"
#include "media/IndexBuilder.h"
#include "media/MediaFile.h"

#include <QCryptographicHash>
#include <QTemporaryDir>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <set>
#include <mutex>

using namespace adscutter;

namespace {

int failures = 0;

[[gnu::format(printf, 1, 2)]] void fail(const char *fmt, ...)
{
    std::va_list args;
    va_start(args, fmt);
    std::fprintf(stderr, "FAIL: ");
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
    va_end(args);
    ++failures;
}

// Counts FFmpeg messages at error level or worse while decoding the output.
std::mutex logMutex;
int loggedErrors = 0;
void logCallback(void *, int level, const char *fmt, va_list args)
{
    if (level > AV_LOG_ERROR)
        return;
    char line[1024];
    std::vsnprintf(line, sizeof(line), fmt, args);
    std::lock_guard lock(logMutex);
    ++loggedErrors;
    std::fprintf(stderr, "  ffmpeg: %s", line);
}

struct SegTiming {
    int64_t offsetUs, startUs, endUs; // output = source + offset
};

// Decodes a whole audio stream: (pts in the stream time base, PCM hash) per frame.
std::vector<std::pair<int64_t, QByteArray>> decodeAudio(const QString &path, int stream, AVRational *timeBase)
{
    std::vector<std::pair<int64_t, QByteArray>> frames;
    QString error;
    FormatContextPtr fmt = openFormatContext(path, &error);
    const AVStream *st = fmt->streams[stream];
    *timeBase = st->time_base;
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    CodecContextPtr ctx(avcodec_alloc_context3(codec));
    avcodec_parameters_to_context(ctx.get(), st->codecpar);
    ctx->pkt_timebase = st->time_base;
    avcodec_open2(ctx.get(), codec, nullptr);
    PacketPtr pkt(av_packet_alloc());
    FramePtr frame(av_frame_alloc());
    auto drain = [&] {
        while (avcodec_receive_frame(ctx.get(), frame.get()) >= 0) {
            QCryptographicHash h(QCryptographicHash::Md5);
            const int bytes = av_get_bytes_per_sample(AVSampleFormat(frame->format));
            const bool planar = av_sample_fmt_is_planar(AVSampleFormat(frame->format));
            const int channels = frame->ch_layout.nb_channels;
            for (int c = 0; c < (planar ? channels : 1); ++c)
                h.addData(QByteArrayView(frame->extended_data[c], frame->nb_samples * bytes * (planar ? 1 : channels)));
            frames.emplace_back(frame->pts, h.result());
            av_frame_unref(frame.get());
        }
    };
    while (av_read_frame(fmt.get(), pkt.get()) >= 0) {
        if (pkt->stream_index == stream) {
            avcodec_send_packet(ctx.get(), pkt.get());
            drain();
        }
        av_packet_unref(pkt.get());
    }
    avcodec_send_packet(ctx.get(), nullptr);
    drain();
    return frames;
}

// For audio whose packets were rewritten (LATM -> AAC): every decoded output
// frame must equal a decoded source frame taken at its segment's offset.
// The first frame after each cut may differ: AAC overlaps consecutive frames.
void checkDecodedAudio(const QString &source, const PacketIndex &index, int sourceStream, const QString &output,
                       int outputStream, const std::vector<SegTiming> &timing)
{
    AVRational srcTb, outTb;
    const auto src = decodeAudio(source, sourceStream, &srcTb);
    const auto out = decodeAudio(output, outputStream, &outTb);

    // Source frame pts -> unified time, through the index.
    const PacketIndex::Stream &s = index.streams()[size_t(sourceStream)];
    std::map<int64_t, int64_t> unified;
    for (uint32_t i = 0; i < s.packets.size(); ++i)
        if (s.packets[i].pts != AV_NOPTS_VALUE)
            unified[s.packets[i].pts] = index.unifiedPts(s, i);
    std::multimap<QByteArray, int64_t> byHash;
    for (const auto &[pts, hash] : src)
        if (auto it = unified.find(pts); it != unified.end())
            byHash.emplace(hash, it->second);

    size_t checked = 0, firstOfSegment = 0, bad = 0;
    const SegTiming *previous = nullptr;
    for (const auto &[pts, hash] : out) {
        const int64_t t = av_rescale_q(pts, outTb, AV_TIME_BASE_Q);
        const SegTiming *seg = nullptr;
        for (const SegTiming &st : timing)
            if (t + 10000 >= st.startUs - 1000 && t < st.endUs + 1000)
                seg = &st;
        if (!seg)
            continue;
        if (seg != previous) {
            previous = seg;
            ++firstOfSegment;
            continue; // overlaps with audio that was cut away
        }
        ++checked;
        bool found = false;
        const auto range = byHash.equal_range(hash);
        for (auto it = range.first; it != range.second; ++it)
            found |= std::llabs(it->second - (t - seg->offsetUs)) <= 1000;
        if (!found && bad++ < 5)
            fail("audio %d: decoded frame at %lld us does not match the source at the same offset", outputStream,
                 static_cast<long long>(t));
    }
    std::printf("  audio %d: %zu decoded frames identical to the source in sync (%zu cut boundaries skipped)\n",
                outputStream, checked - bad, firstOfSegment);
    if (checked == 0)
        fail("audio %d: no frames checked", outputStream);
}

QByteArray md5(const uint8_t *data, int size)
{
    return QCryptographicHash::hash(QByteArrayView(data, size), QCryptographicHash::Md5);
}

std::vector<int> parseList(const char *arg)
{
    std::vector<int> out;
    for (const QString &part : QString::fromLatin1(arg).split(QLatin1Char(','), Qt::SkipEmptyParts))
        out.push_back(part.toInt());
    return out;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s FILE SEGMENTS [STREAMS]\n", argv[0]);
        return 2;
    }
    av_log_set_level(AV_LOG_ERROR);
    av_log_set_callback([](void *, int, const char *, va_list) {}); // quiet while preparing
    const QString path = QString::fromLocal8Bit(argv[1]);

    MediaFile file;
    QString error;
    if (!file.open(path, &error)) {
        fail("open: %s", qPrintable(error));
        return 1;
    }
    const int videoIndex = file.bestVideoStream();
    std::vector<StreamInfo> streams = file.streams();
    FormatContextPtr fmt = file.takeFormatContext();
    std::shared_ptr<const PacketIndex> index(buildIndex(fmt.get(), {}, &error).release());
    fmt.reset();
    const PacketIndex::Stream &video = *index->stream(videoIndex);

    // Segments, snapped like the UI does.
    std::vector<Segment> segments;
    for (const QString &part : QString::fromLatin1(argv[2]).split(QLatin1Char(','))) {
        const QStringList ends = part.split(QLatin1Char('-'));
        const auto in = uint32_t(ends.value(0).toUInt());
        segments.push_back({PacketIndex::keyframeAtOrBefore(video, in), uint32_t(ends.value(1).toUInt())});
    }

    ExportSettings settings;
    settings.videoStream = videoIndex;
    settings.segments = segments;
    if (argc >= 4) {
        settings.streams = parseList(argv[3]);
    } else {
        for (const StreamInfo &s : streams)
            if (s.mkvCompatible)
                settings.streams.push_back(s.index);
    }
    const AVRational fr = streams[size_t(videoIndex)].frameRate;
    settings.frameDurationUs = av_rescale(AV_TIME_BASE, fr.den, fr.num);
    const int64_t frameUs = settings.frameDurationUs;

    QTemporaryDir dir;
    settings.outputPath = dir.filePath(QStringLiteral("out.mkv"));
    ExportReport report;
    if (!exportMatroska(path, *index, settings, {}, &report, &error)) {
        fail("export: %s", qPrintable(error));
        return 1;
    }
    std::printf("%s: exported %zu segment(s), %.3f s, %llu packets, %u parameter sets inserted, %u CRA->BLA, "
                "%u I->IDR\n",
                argv[1], report.segments.size(), report.durationUs / 1e6,
                static_cast<unsigned long long>(report.packetsWritten), report.paramSetsInserted,
                report.blaConversions, report.idrConversions);
    for (const auto &s : report.segments)
        std::printf("  segment %u-%u: %.3f s, %u extra frame(s)\n", s.frames.in, s.frames.out, s.durationUs / 1e6,
                    s.extraFrames);
    for (const QString &w : report.warnings)
        std::printf("  warning: %s\n", qPrintable(w));
    // Debug aid: KEEP_EXPORT=/path/out.mkv keeps a copy of the exported file.
    if (qEnvironmentVariableIsSet("KEEP_EXPORT"))
        QFile::copy(settings.outputPath, qEnvironmentVariable("KEEP_EXPORT"));
    if (QFile::exists(settings.outputPath + QStringLiteral(".part")))
        fail("temporary file left behind");

    // --- Streams -----------------------------------------------------------
    FormatContextPtr out = openFormatContext(settings.outputPath, &error);
    if (!out) {
        fail("cannot open output: %s", qPrintable(error));
        return 1;
    }
    if (out->nb_streams != settings.streams.size())
        fail("output has %u streams, expected %zu", out->nb_streams, settings.streams.size());
    int outVideo = -1;
    std::vector<int> outAudio; // output index -> source index
    std::map<int, int> sourceOf;
    std::set<int> subtitles; // output indices of subtitle streams (checked like audio, but sparse)
    for (unsigned i = 0; i < out->nb_streams && i < settings.streams.size(); ++i) {
        const StreamInfo &src = streams[size_t(settings.streams[i])];
        const AVStream *st = out->streams[i];
        sourceOf[int(i)] = src.index;
        const bool latmToAac = src.codecId == AV_CODEC_ID_AAC_LATM && st->codecpar->codec_id == AV_CODEC_ID_AAC;
        if (st->codecpar->codec_id != src.codecId && !latmToAac)
            fail("stream %u: codec changed", i);
        const AVDictionaryEntry *lang = av_dict_get(st->metadata, "language", nullptr, 0);
        if (!src.language.isEmpty() && (!lang || src.language != QString::fromUtf8(lang->value)))
            fail("stream %u: language not preserved", i);
        if (src.index == videoIndex)
            outVideo = int(i);
        if (src.type == AVMEDIA_TYPE_AUDIO || src.type == AVMEDIA_TYPE_SUBTITLE)
            outAudio.push_back(int(i));
        if (src.type == AVMEDIA_TYPE_SUBTITLE)
            subtitles.insert(int(i));
    }
    out.reset();

    // --- Video pictures ----------------------------------------------------------
    std::vector<uint32_t> expected; // source display positions, in output order
    for (const auto &s : report.segments) {
        for (uint32_t pos = s.frames.in; pos <= s.frames.out + s.extraFrames; ++pos)
            expected.push_back(pos);
    }
    const test::DecodedVideo source = test::decodeAll(path, videoIndex);
    av_log_set_callback(logCallback);
    const test::DecodedVideo result = test::decodeAll(settings.outputPath, outVideo);
    av_log_set_callback([](void *, int, const char *, va_list) {});
    if (loggedErrors)
        fail("%d error message(s) while decoding the video", loggedErrors);

    std::printf("  video: %zu frames decoded, %zu expected\n", result.hashes.size(), expected.size());
    if (result.hashes.size() != expected.size())
        fail("frame count differs");
    size_t mismatches = 0;
    for (size_t i = 0; i < std::min(result.hashes.size(), expected.size()); ++i) {
        if (result.hashes[i] != source.hashes[expected[i]] && mismatches++ < 5)
            fail("output frame %zu is not source frame %u", i, expected[i]);
    }

    // --- Video timing ---------------------------------------------------------------
    std::vector<int64_t> outUs(result.pts.size());
    for (size_t i = 0; i < result.pts.size(); ++i)
        outUs[i] = av_rescale_q(result.pts[i], result.timeBase, AV_TIME_BASE_Q);
    std::vector<SegTiming> timing;
    size_t frame = 0;
    for (const auto &s : report.segments) {
        const size_t count = s.frames.out + s.extraFrames - s.frames.in + 1;
        if (frame + count > outUs.size())
            break;
        SegTiming t{};
        t.offsetUs = outUs[frame] - index->frameTime(video, s.frames.in);
        t.startUs = outUs[frame];
        t.endUs = outUs[frame + count - 1] + frameUs;
        for (size_t k = 0; k < count; ++k) {
            const int64_t offset = outUs[frame + k] - index->frameTime(video, s.frames.in + uint32_t(k));
            if (std::llabs(offset - t.offsetUs) > 1000)
                fail("segment at %u: frame %zu shifted by %lld us", s.frames.in, k,
                     static_cast<long long>(offset - t.offsetUs));
        }
        if (!timing.empty()) {
            const int64_t gap = t.startUs - timing.back().endUs;
            if (gap < -1000 || gap > 2000)
                fail("gap of %lld us before segment at %u", static_cast<long long>(gap), s.frames.in);
        }
        timing.push_back(t);
        frame += count;
    }
    for (size_t i = 1; i < outUs.size(); ++i) {
        if (outUs[i] <= outUs[i - 1]) {
            fail("video timestamps not increasing at frame %zu", i);
            break;
        }
    }

    // --- Audio --------------------------------------------------------------------------
    // Source audio packets by content.
    std::map<int, std::multimap<QByteArray, int64_t>> sourceAudio; // source stream -> md5 -> unified pts
    {
        FormatContextPtr in = openFormatContext(path, &error);
        PacketPtr pkt(av_packet_alloc());
        std::map<int, uint32_t> counter;
        while (av_read_frame(in.get(), pkt.get()) >= 0) {
            const int s = pkt->stream_index;
            const uint32_t i = counter[s]++;
            const AVMediaType type = index->streams()[size_t(s)].type;
            if ((type == AVMEDIA_TYPE_AUDIO || type == AVMEDIA_TYPE_SUBTITLE) && i < index->streams()[size_t(s)].packets.size())
                sourceAudio[s].emplace(md5(pkt->data, pkt->size), index->unifiedPts(index->streams()[size_t(s)], i));
            av_packet_unref(pkt.get());
        }
    }
    {
        FormatContextPtr o = openFormatContext(settings.outputPath, &error);
        PacketPtr pkt(av_packet_alloc());
        std::map<int, std::vector<std::pair<int64_t, int64_t>>> spans; // out stream -> (start, end) us
        std::map<int, size_t> unmatched, matched;
        while (av_read_frame(o.get(), pkt.get()) >= 0) {
            const int s = pkt->stream_index;
            if (std::find(outAudio.begin(), outAudio.end(), s) == outAudio.end()) {
                av_packet_unref(pkt.get());
                continue;
            }
            const AVStream *st = o->streams[s];
            const int64_t start = av_rescale_q(pkt->pts, st->time_base, AV_TIME_BASE_Q);
            const int64_t end = start + av_rescale_q(pkt->duration, st->time_base, AV_TIME_BASE_Q);
            if (!spans[s].empty() && start <= spans[s].back().first)
                fail("audio stream %d: timestamps not increasing", s);
            spans[s].emplace_back(start, end);

            // Which segment: the one containing the packet's middle (start
            // for subtitles, which may stay on screen across a cut).
            const int64_t mid = subtitles.count(s) ? start : (start + end) / 2;
            const SegTiming *seg = nullptr;
            for (const SegTiming &t : timing)
                if (mid >= t.startUs - 1000 && mid < t.endUs + 1000)
                    seg = &t;
            if (!seg) {
                fail("audio stream %d: packet at %lld us outside every segment", s, static_cast<long long>(start));
            } else {
                // Same bytes at exactly the expected source time?
                const int64_t wantSource = start - seg->offsetUs;
                const auto range = sourceAudio[sourceOf[s]].equal_range(md5(pkt->data, pkt->size));
                bool found = false, contentKnown = range.first != range.second;
                for (auto it = range.first; it != range.second; ++it)
                    found |= std::llabs(it->second - wantSource) <= 1000;
                if (contentKnown && !found)
                    fail("audio stream %d: packet at %lld us not taken in sync with its segment", s,
                         static_cast<long long>(start));
                ++(contentKnown ? matched[s] : unmatched[s]);
            }
            av_packet_unref(pkt.get());
        }

        for (int s : outAudio) {
            const auto &sp = spans[s];
            if (subtitles.count(s)) {
                // Every source cue starting inside a kept part, and only those.
                const PacketIndex::Stream &src = index->streams()[size_t(sourceOf[s])];
                size_t expected = 0;
                for (const auto &seg : report.segments)
                    for (uint32_t pos = 0; pos < src.displayOrder.size(); ++pos) {
                        const int64_t t = index->unifiedPts(src, src.displayOrder[pos]);
                        expected += t >= seg.sourceStartUs && t < seg.sourceStartUs + seg.durationUs;
                    }
                std::printf("  subtitles %d: %zu cues (%zu matched to source bytes in sync), %zu expected\n", s,
                            sp.size(), matched[s], expected);
                if (sp.size() != expected || matched[s] != expected)
                    fail("subtitles %d: wrong cues", s);
                continue;
            }
            // Longest frame: some codecs (Vorbis) vary the frame length.
            int64_t dur = 0;
            for (const auto &[a, b] : sp)
                dur = std::max(dur, b - a);
            std::printf("  audio %d: %zu packets (%zu matched to source bytes), longest frame %.1f ms\n", s, sp.size(),
                        matched[s], dur / 1e3);
            if (matched[s] == 0)
                checkDecodedAudio(path, *index, sourceOf[s], settings.outputPath, s, timing);
            // Coverage of each segment within half a frame (+ rounding).
            for (const SegTiming &t : timing) {
                int64_t first = INT64_MAX, last = INT64_MIN;
                for (const auto &[a, b] : sp) {
                    const int64_t mid = (a + b) / 2;
                    if (mid >= t.startUs - 1000 && mid < t.endUs + 1000) {
                        first = std::min(first, a);
                        last = std::max(last, b);
                    }
                }
                const int64_t tolerance = dur / 2 + 1500;
                if (first == INT64_MAX || std::llabs(first - t.startUs) > tolerance
                    || std::llabs(last - t.endUs) > tolerance)
                    fail("audio %d: segment %.3f-%.3f s covered by %.3f-%.3f s", s, t.startUs / 1e6,
                         t.endUs / 1e6, first / 1e6, last / 1e6);
            }
        }
    }

    // Decode the audio too, to catch broken packets.
    {
        av_log_set_callback(logCallback);
        loggedErrors = 0;
        FormatContextPtr o = openFormatContext(settings.outputPath, &error);
        std::map<int, CodecContextPtr> decoders;
        for (int s : outAudio) {
            if (subtitles.count(s))
                continue; // subtitles use a different decoding API
            const AVCodec *c = avcodec_find_decoder(o->streams[s]->codecpar->codec_id);
            CodecContextPtr ctx(avcodec_alloc_context3(c));
            avcodec_parameters_to_context(ctx.get(), o->streams[s]->codecpar);
            avcodec_open2(ctx.get(), c, nullptr);
            decoders[s] = std::move(ctx);
        }
        PacketPtr pkt(av_packet_alloc());
        FramePtr fr2(av_frame_alloc());
        while (av_read_frame(o.get(), pkt.get()) >= 0) {
            if (auto it = decoders.find(pkt->stream_index); it != decoders.end()) {
                avcodec_send_packet(it->second.get(), pkt.get());
                while (avcodec_receive_frame(it->second.get(), fr2.get()) >= 0)
                    av_frame_unref(fr2.get());
            }
            av_packet_unref(pkt.get());
        }
        av_log_set_callback([](void *, int, const char *, va_list) {});
        if (loggedErrors)
            fail("%d error message(s) while decoding the audio", loggedErrors);
    }

    if (failures == 0)
        std::printf("  OK\n");
    return failures == 0 ? 0 : 1;
}
