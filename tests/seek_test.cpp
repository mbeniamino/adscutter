// Verifies frame-accurate random access.
//
// usage: seek_test FILE
//
// Decodes the main video stream sequentially as a reference (a hash per
// frame), then checks that RandomAccessDecoder returns exactly the same
// pictures for:
//  - stepping backwards through every GOP (served from the GOP cache),
//  - stepping forwards through the whole file (one decoding run, no seeks),
//  - the frames just before each cut point (open-GOP leading pictures,
//    which are decoded after the cut point but shown before it),
//  - random jumps, with a normal and with a tiny cache,
// and that deinterlacing keeps the first field untouched.

#include "media/Deinterlace.h"
#include "media/IndexBuilder.h"
#include "media/MediaFile.h"
#include "media/RandomAccessDecoder.h"
#include "TestUtil.h"

#include <QCryptographicHash>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>
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

using test::hashFrame;

std::vector<QByteArray> referenceHashes(const QString &path, int streamIndex)
{
    return test::decodeAll(path, streamIndex).hashes;
}

struct Checker {
    RandomAccessDecoder &decoder;
    const std::vector<QByteArray> &reference;
    const char *phase;
    size_t checked = 0;

    void check(uint32_t pos)
    {
        QString error;
        FramePtr frame = decoder.frameAt(pos, &error);
        ++checked;
        if (!frame) {
            fail("%s: frame %u: %s", phase, pos, qPrintable(error));
            return;
        }
        if (hashFrame(frame.get()) != reference[pos])
            fail("%s: frame %u differs from sequential decoding", phase, pos);
    }
};

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s FILE\n", argv[0]);
        return 2;
    }
    av_log_set_level(AV_LOG_FATAL);
    const QString path = QString::fromLocal8Bit(argv[1]);

    MediaFile file;
    QString error;
    if (!file.open(path, &error)) {
        fail("open: %s", qPrintable(error));
        return 1;
    }
    const int videoIndex = file.bestVideoStream();
    FormatContextPtr fmt = file.takeFormatContext();
    std::shared_ptr<const PacketIndex> index(buildIndex(fmt.get(), {}, &error).release());
    fmt.reset();
    if (!index) {
        fail("index: %s", qPrintable(error));
        return 1;
    }
    const PacketIndex::Stream &video = *index->stream(videoIndex);
    const uint32_t frames = uint32_t(video.displayOrder.size());

    const std::vector<QByteArray> reference = referenceHashes(path, videoIndex);
    if (reference.size() != frames) {
        fail("reference decoding produced %zu frames, index has %u", reference.size(), frames);
        return 1;
    }

    RandomAccessDecoder decoder;
    if (!decoder.open(path, videoIndex, index, &error)) {
        fail("decoder: %s", qPrintable(error));
        return 1;
    }

    // 1. Backwards through every GOP, starting from its last frame.
    {
        Checker c{decoder, reference, "backward"};
        const auto before = decoder.stats();
        for (size_t k = 0; k < video.keyframes.size(); ++k) {
            const uint32_t first = video.keyframes[k];
            const uint32_t end = k + 1 < video.keyframes.size() ? video.keyframes[k + 1] : frames;
            for (uint32_t pos = end; pos-- > first;)
                c.check(pos);
        }
        const auto after = decoder.stats();
        std::printf("backward: %zu frames, %llu seeks, %llu cache hits\n", c.checked,
                    static_cast<unsigned long long>(after.seeks - before.seeks),
                    static_cast<unsigned long long>(after.cacheHits - before.cacheHits));
        // Each GOP needs one seek at most; the rest comes from the cache.
        if (after.seeks - before.seeks > video.keyframes.size())
            fail("backward stepping needed %llu seeks for %zu GOPs",
                 static_cast<unsigned long long>(after.seeks - before.seeks), video.keyframes.size());
    }

    // 2. Forwards through the whole file with a fresh decoder: one run.
    {
        RandomAccessDecoder fresh;
        fresh.open(path, videoIndex, index, &error);
        Checker c{fresh, reference, "forward"};
        for (uint32_t pos = 0; pos < frames; ++pos)
            c.check(pos);
        std::printf("forward: %zu frames, %llu seeks\n", c.checked,
                    static_cast<unsigned long long>(fresh.stats().seeks));
        if (fresh.stats().seeks != 1)
            fail("forward stepping should not seek (%llu seeks)",
                 static_cast<unsigned long long>(fresh.stats().seeks));
    }

    // 3. Frames right before each cut point, cold cache each time.
    {
        Checker c{decoder, reference, "leading"};
        decoder.setCacheBudget(0);
        for (size_t k = 1; k < video.keyframes.size(); ++k) {
            const uint32_t key = video.keyframes[k];
            for (uint32_t back = 1; back <= 3 && back <= key; ++back) {
                c.check(key - back);
                c.check(video.keyframes[k - 1]); // move away to force a new run
            }
        }
        std::printf("leading: %zu frames\n", c.checked);
    }

    // 4. Random jumps, with a normal and with a tiny cache.
    for (const size_t budget : {size_t(512) << 20, size_t(0)}) {
        decoder.setCacheBudget(budget);
        Checker c{decoder, reference, budget ? "random" : "random/no-cache"};
        uint32_t state = 12345;
        for (int i = 0; i < 300; ++i) {
            state = state * 1103515245u + 12345u;
            c.check((state >> 8) % frames);
        }
        std::printf("%s: %zu frames, cache holds %zu\n", c.phase, c.checked, decoder.cachedFrames());
    }

    // 5. Deinterlacing keeps the first field's lines.
    {
        decoder.setCacheBudget(size_t(64) << 20);
        FramePtr frame = decoder.frameAt(frames / 2, &error);
        if (frame && (frame->flags & AV_FRAME_FLAG_INTERLACED)) {
            FramePtr d = deinterlaceFrame(frame.get());
            if (!d) {
                fail("deinterlace: unsupported format %s",
                     av_get_pix_fmt_name(AVPixelFormat(frame->format)));
            } else {
                const int kept = (frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) ? 0 : 1;
                bool same = d->width == frame->width && d->height == frame->height;
                bool changed = false;
                for (int y = 0; same && y < frame->height; ++y) {
                    const bool equal = std::memcmp(frame->data[0] + ptrdiff_t(frame->linesize[0]) * y,
                                                   d->data[0] + ptrdiff_t(d->linesize[0]) * y,
                                                   size_t(frame->width)) == 0;
                    if ((y & 1) == kept)
                        same = equal;
                    else
                        changed |= !equal;
                }
                if (!same)
                    fail("deinterlace altered the kept field");
                if (!changed)
                    fail("deinterlace did not touch the other field");
                std::printf("deinterlace: %s field kept\n", kept == 0 ? "top" : "bottom");
            }
        }
    }

    return failures == 0 ? 0 : 1;
}
