// Builds the packet index of a file and verifies it.
//
// usage: index_test FILE FRAMES DURATION_S DISCONTINUITIES [--raw-timestamps]
//
// --raw-timestamps disables libavformat's own correction of 33-bit
// wraparounds, so that the index has to handle them by itself.
//
// Checks:
//  - the main video stream has FRAMES navigable frames, the timeline lasts
//    DURATION_S seconds and has DISCONTINUITIES timestamp discontinuities;
//  - unified frame times increase steadily (wraparounds / discontinuities
//    are hidden) and audio stays aligned with video;
//  - the cut points match the frames the FFmpeg decoder flags as keyframes
//    (the same information `ffprobe -show_frames` reports);
//  - the index survives a cache round trip, and a stale cache is rejected.

#include "media/IndexBuilder.h"
#include "media/MediaFile.h"
#include "media/PacketIndex.h"

#include <QElapsedTimer>
#include <QTemporaryDir>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

extern "C" {
#include <libavutil/log.h>
}

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

// Decodes the whole stream and returns the pts of the frames the decoder
// marks as keyframes, plus the number of frames decoded.
std::set<int64_t> decoderKeyframes(const QString &path, int streamIndex, int64_t mask, size_t *frames)
{
    std::set<int64_t> keys;
    *frames = 0;
    QString error;
    FormatContextPtr fmt = openFormatContext(path, &error);
    if (!fmt) {
        fail("reopen: %s", qPrintable(error));
        return keys;
    }
    const AVStream *st = fmt->streams[streamIndex];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    CodecContextPtr ctx(avcodec_alloc_context3(codec));
    avcodec_parameters_to_context(ctx.get(), st->codecpar);
    ctx->pkt_timebase = st->time_base;
    if (avcodec_open2(ctx.get(), codec, nullptr) < 0) {
        fail("cannot open decoder");
        return keys;
    }

    PacketPtr pkt(av_packet_alloc());
    FramePtr frame(av_frame_alloc());
    auto drain = [&] {
        while (avcodec_receive_frame(ctx.get(), frame.get()) >= 0) {
            ++*frames;
            if (frame->flags & AV_FRAME_FLAG_KEY)
                keys.insert(frame->pts & mask);
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
    return keys;
}

bool samePackets(const PacketIndex &a, const PacketIndex &b)
{
    if (a.streams().size() != b.streams().size())
        return false;
    for (size_t i = 0; i < a.streams().size(); ++i) {
        const auto &sa = a.streams()[i];
        const auto &sb = b.streams()[i];
        if (sa.packets.size() != sb.packets.size() || sa.segments.size() != sb.segments.size()
            || sa.displayOrder != sb.displayOrder || sa.keyframes != sb.keyframes)
            return false;
        if (!sa.packets.empty()
            && std::memcmp(sa.packets.data(), sb.packets.data(), sa.packets.size() * sizeof(PacketIndex::Packet)) != 0)
            return false;
        for (size_t s = 0; s < sa.segments.size(); ++s) {
            if (sa.segments[s].firstPacket != sb.segments[s].firstPacket
                || sa.segments[s].offsetUs != sb.segments[s].offsetUs)
                return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    const bool rawTimestamps = argc == 6 && std::strcmp(argv[5], "--raw-timestamps") == 0;
    if (argc != 5 && !rawTimestamps) {
        std::fprintf(stderr, "usage: %s FILE FRAMES DURATION_S DISCONTINUITIES [--raw-timestamps]\n", argv[0]);
        return 2;
    }
    av_log_set_level(AV_LOG_FATAL);
    const QString path = QString::fromLocal8Bit(argv[1]);
    const size_t expectFrames = std::strtoul(argv[2], nullptr, 10);
    const double expectDuration = std::atof(argv[3]);
    const size_t expectDiscontinuities = std::strtoul(argv[4], nullptr, 10);

    MediaFile file;
    QString error;
    if (!file.open(path, &error)) {
        fail("open: %s", qPrintable(error));
        return 1;
    }
    const int videoIndex = file.bestVideoStream();
    const int wrapBits = file.formatContext()->streams[videoIndex]->pts_wrap_bits;

    QElapsedTimer timer;
    timer.start();
    FormatContextPtr fmt = file.takeFormatContext();
    if (rawTimestamps) {
        AVDictionary *opts = nullptr;
        av_dict_set(&opts, "correct_ts_overflow", "0", 0);
        fmt = openFormatContext(path, &error, opts);
        av_dict_free(&opts);
        if (!fmt) {
            fail("reopen: %s", qPrintable(error));
            return 1;
        }
    }
    auto index = buildIndex(fmt.get(), {}, &error);
    fmt.reset();
    if (!index) {
        fail("index: %s", qPrintable(error));
        return 1;
    }
    std::printf("%s: indexed in %lld ms, duration %.3f s, %zu discontinuities\n", argv[1],
                static_cast<long long>(timer.elapsed()), index->durationUs() / 1e6, index->discontinuities());
    for (const auto &s : index->streams()) {
        std::printf("  #%d %s: %zu packets, %zu segments, %zu shown, %zu cut points%s\n", s.index,
                    av_get_media_type_string(s.type), s.packets.size(), s.segments.size(),
                    s.displayOrder.size(), s.keyframes.size(), s.intraFallback ? " (intra fallback)" : "");
    }

    const PacketIndex::Stream *video = index->stream(videoIndex);
    if (!video) {
        fail("no video stream in index");
        return 1;
    }

    // --- Shape of the timeline --------------------------------------------
    if (video->displayOrder.size() != expectFrames)
        fail("expected %zu frames, got %zu", expectFrames, video->displayOrder.size());
    if (std::abs(index->durationUs() / 1e6 - expectDuration) > 0.15)
        fail("expected duration %.3f s, got %.3f s", expectDuration, index->durationUs() / 1e6);
    if (index->discontinuities() != expectDiscontinuities)
        fail("expected %zu discontinuities, got %zu", expectDiscontinuities, index->discontinuities());
    if (video->keyframes.empty() || video->keyframes.front() != 0)
        fail("timeline must start on a cut point");

    // Frame steps: one frame duration, except across discontinuities where
    // the gap may be up to half a second.
    const AVRational fr = file.streams()[videoIndex].frameRate;
    const int64_t frameUs = fr.num > 0 ? av_rescale(AV_TIME_BASE, fr.den, fr.num) : 40000;
    size_t irregular = 0;
    for (size_t i = 1; i < video->displayOrder.size(); ++i) {
        const int64_t step = index->unifiedPts(*video, video->displayOrder[i])
                             - index->unifiedPts(*video, video->displayOrder[i - 1]);
        if (step <= 0 || step > 500000) {
            fail("frame %zu: step of %lld us", i, static_cast<long long>(step));
        } else if (std::llabs(step - frameUs) > frameUs / 2) {
            ++irregular;
        }
    }
    if (irregular > expectDiscontinuities)
        fail("%zu irregular frame steps", irregular);

    // Audio must cover the same span as video, i.e. it got the same offsets.
    for (const auto &s : index->streams()) {
        if (s.type != AVMEDIA_TYPE_AUDIO || s.displayOrder.empty())
            continue;
        const int64_t aStart = index->unifiedPts(s, s.displayOrder.front());
        const int64_t aEnd = index->unifiedPts(s, s.displayOrder.back());
        const int64_t vStart = index->unifiedPts(*video, video->displayOrder.front());
        const int64_t vEnd = index->unifiedPts(*video, video->displayOrder.back());
        if (std::llabs(aStart - vStart) > 300000 || std::llabs(aEnd - vEnd) > 300000)
            fail("audio #%d spans %.3f-%.3f s, video %.3f-%.3f s", s.index, aStart / 1e6, aEnd / 1e6,
                 vStart / 1e6, vEnd / 1e6);
    }

    // --- Cut points vs decoder keyframes -----------------------------------
    const int64_t mask = wrapBits > 0 && wrapBits < 63 ? (int64_t(1) << wrapBits) - 1 : INT64_MAX;
    std::set<int64_t> ours;
    for (uint32_t pos : video->keyframes)
        ours.insert(video->packets[video->displayOrder[pos]].pts & mask);
    size_t decodedFrames = 0;
    const std::set<int64_t> theirs = decoderKeyframes(path, videoIndex, mask, &decodedFrames);
    std::printf("  cut points: %zu, decoder keyframes: %zu, decoded frames: %zu\n", ours.size(),
                theirs.size(), decodedFrames);
    if (ours != theirs) {
        fail("cut points differ from decoder keyframes");
        for (int64_t t : ours)
            if (!theirs.count(t))
                std::fprintf(stderr, "  only in index:   %lld\n", static_cast<long long>(t));
        for (int64_t t : theirs)
            if (!ours.count(t))
                std::fprintf(stderr, "  only in decoder: %lld\n", static_cast<long long>(t));
    }
    if (decodedFrames != video->displayOrder.size())
        fail("decoder produced %zu frames, index has %zu", decodedFrames, video->displayOrder.size());

    // --- Cache round trip ---------------------------------------------------
    QTemporaryDir dir;
    const QString cacheFile = PacketIndex::cachePath(path, dir.path());
    const FileKey key = FileKey::of(path);
    if (!index->save(cacheFile, key, &error)) {
        fail("save: %s", qPrintable(error));
    } else {
        auto loaded = PacketIndex::load(cacheFile, key, &error);
        if (!loaded)
            fail("load: %s", qPrintable(error));
        else if (!samePackets(*index, *loaded) || loaded->durationUs() != index->durationUs())
            fail("loaded index differs");
        FileKey stale = key;
        stale.mtimeMs += 1000;
        if (PacketIndex::load(cacheFile, stale, &error))
            fail("stale cache accepted");
    }

    return failures == 0 ? 0 : 1;
}
