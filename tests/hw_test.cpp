// Checks hardware decoding (Vulkan or VAAPI) against software decoding.
//
// usage: hw_test FILE vulkan|vaapi
//
// Random access with the given backend must return the same pictures as a
// sequential software decode: stepping backwards through every GOP and 60
// random jumps, and back into each GOP after the next. Frames are compared after converting both to planar YUV;
// H.264 and HEVC decoding is bit exact by specification, so they must be
// identical (MPEG-2 allows IDCT differences: it must then be clearly the
// right frame, much closer to it than to its neighbours). Interlaced frames are also deinterlaced in the hardware format
// (NV12) to check that the kept field is untouched.
//
// Exits with 77 (skipped) when the backend is not available for the file.

#include "TestUtil.h"
#include "media/Deinterlace.h"
#include "media/IndexBuilder.h"
#include "media/MediaFile.h"
#include "media/RandomAccessDecoder.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace adscutter;

namespace {

// Same picture as planar YUV, whatever the layout (NV12 -> 420p).
FramePtr toPlanar(const AVFrame *f)
{
    if (av_pix_fmt_count_planes(AVPixelFormat(f->format)) == 3)
        return FramePtr(av_frame_clone(f));
    FramePtr planar(av_frame_alloc());
    planar->format = AV_PIX_FMT_YUV420P;
    planar->width = f->width;
    planar->height = f->height;
    av_frame_get_buffer(planar.get(), 0);
    for (int y = 0; y < f->height; ++y)
        std::memcpy(planar->data[0] + ptrdiff_t(planar->linesize[0]) * y, f->data[0] + ptrdiff_t(f->linesize[0]) * y,
                    size_t(f->width));
    for (int y = 0; y < (f->height + 1) / 2; ++y) {
        const uint8_t *uv = f->data[1] + ptrdiff_t(f->linesize[1]) * y;
        uint8_t *u = planar->data[1] + ptrdiff_t(planar->linesize[1]) * y;
        uint8_t *v = planar->data[2] + ptrdiff_t(planar->linesize[2]) * y;
        for (int x = 0; x < (f->width + 1) / 2; ++x) {
            u[x] = uv[2 * x];
            v[x] = uv[2 * x + 1];
        }
    }
    return planar;
}

// Luma PSNR in dB (100 = identical).
double psnr(const AVFrame *a, const AVFrame *b)
{
    double sse = 0;
    for (int y = 0; y < a->height; ++y) {
        const uint8_t *pa = a->data[0] + ptrdiff_t(a->linesize[0]) * y;
        const uint8_t *pb = b->data[0] + ptrdiff_t(b->linesize[0]) * y;
        for (int x = 0; x < a->width; ++x) {
            const double d = double(pa[x]) - double(pb[x]);
            sse += d * d;
        }
    }
    if (sse == 0)
        return 100;
    return 10 * std::log10(255.0 * 255.0 * a->width * a->height / sse);
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s FILE vulkan|vaapi\n", argv[0]);
        return 2;
    }
    // HWTEST_LOG=1 shows FFmpeg's warnings (to investigate a failure).
    av_log_set_level(qEnvironmentVariableIsSet("HWTEST_LOG") ? AV_LOG_WARNING : AV_LOG_FATAL);
    const QString path = QString::fromLocal8Bit(argv[1]);
    const HwPreference backend = hwPreferenceFromString(QString::fromLatin1(argv[2]));

    MediaFile file;
    QString error;
    if (!file.open(path, &error)) {
        std::fprintf(stderr, "FAIL open: %s\n", qPrintable(error));
        return 1;
    }
    const int videoIndex = file.bestVideoStream();
    FormatContextPtr fmt = file.takeFormatContext();
    std::shared_ptr<const PacketIndex> index(buildIndex(fmt.get(), {}, &error).release());
    fmt.reset();
    const PacketIndex::Stream &video = *index->stream(videoIndex);
    const auto frames = uint32_t(video.displayOrder.size());

    RandomAccessDecoder decoder;
    if (!decoder.open(path, videoIndex, index, &error, backend)) {
        std::fprintf(stderr, "FAIL decoder: %s\n", qPrintable(error));
        return 1;
    }
    FramePtr first = decoder.frameAt(0, &error);
    if (!first) {
        std::fprintf(stderr, "FAIL first frame: %s\n", qPrintable(error));
        return 1;
    }
    std::printf("%s with %s: decoding in %s (%s)\n", argv[1], argv[2], qPrintable(decoder.decoderName()),
                av_get_pix_fmt_name(AVPixelFormat(first->format)));
    if (decoder.decoderName() == QStringLiteral("software")) {
        std::printf("  %s not available for this stream: skipped\n", argv[2]);
        return 77;
    }

    const auto reference = test::decodeAll(path, videoIndex).hashes;
    if (reference.size() != frames) {
        std::fprintf(stderr, "FAIL: reference has %zu frames, index %u\n", reference.size(), frames);
        return 1;
    }

    // Software frames to compare with when a hardware frame is not bit exact.
    RandomAccessDecoder software;
    software.open(path, videoIndex, index, &error, HwPreference::Off);
    const bool exactCodec = video.codecId == AV_CODEC_ID_H264 || video.codecId == AV_CODEC_ID_HEVC;

    int failures = 0, checked = 0, exact = 0;
    double worstPsnr = 100, bestNeighbour = 0;
    int64_t previous = -1;
    auto check = [&](uint32_t pos, const char *phase) {
        const bool wasFailed = decoder.hardwareFailed();
        FramePtr f = decoder.frameAt(pos, &error);
        ++checked;
        if (!wasFailed && decoder.hardwareFailed())
            std::printf("  fallback while checking frame %u (%s phase, after frame %lld)\n", pos, phase,
                        static_cast<long long>(previous));
        previous = pos;
        if (!f) {
            if (failures++ < 5)
                std::fprintf(stderr, "FAIL %s: frame %u: %s\n", phase, pos, qPrintable(error));
            return;
        }
        FramePtr planar = toPlanar(f.get());
        if (test::hashFrame(planar.get()) == reference[pos]) {
            ++exact;
            return;
        }
        // Not bit exact: it must still be clearly this frame and not a neighbour.
        FramePtr want = software.frameAt(pos, &error);
        const double same = psnr(planar.get(), want.get());
        double neighbour = 0;
        for (int64_t n : {int64_t(pos) - 1, int64_t(pos) + 1}) {
            if (n >= 0 && n < int64_t(frames)) {
                FramePtr other = software.frameAt(uint32_t(n), &error);
                neighbour = std::max(neighbour, psnr(planar.get(), other.get()));
            }
        }
        worstPsnr = std::min(worstPsnr, same);
        bestNeighbour = std::max(bestNeighbour, neighbour);
        // MPEG-2 allows decoder IDCT differences; H.264/HEVC are bit exact.
        const bool ok = !exactCodec && same >= 40 && same > neighbour + 6;
        if (!ok && failures++ < 5)
            std::fprintf(stderr, "FAIL %s: frame %u: PSNR %.1f dB vs software (neighbours %.1f dB)\n", phase, pos,
                         same, neighbour);
    };

    for (size_t k = 0; k < video.keyframes.size(); ++k) {
        const uint32_t end = k + 1 < video.keyframes.size() ? video.keyframes[k + 1] : frames;
        for (uint32_t pos = end; pos-- > video.keyframes[k];)
            check(pos, "backward");
    }
    decoder.setCacheBudget(0);
    // Back into each GOP after decoding the next one: the seek must not reuse
    // the decoder's state (Vulkan H.264 interlaced once decoded wrong pictures
    // from stale references there). The last frame needs the whole GOP.
    for (size_t k = 0; k + 1 < video.keyframes.size(); ++k) {
        check(video.keyframes[k + 1], "back");
        check(video.keyframes[k + 1] - 1, "back");
    }
    uint32_t state = 4242;
    for (int i = 0; i < 60; ++i) { // each jump decodes up to a whole GOP
        state = state * 1103515245u + 12345u;
        check((state >> 8) % frames, "random");
    }
    if (decoder.hardwareFailed())
        std::printf("  note: fell back to software: %s\n", qPrintable(decoder.hardwareFailure()));

    // Deinterlacing in the hardware frame format keeps the first field.
    FramePtr mid = decoder.frameAt(frames / 2, &error);
    if (mid && (mid->flags & AV_FRAME_FLAG_INTERLACED)) {
        FramePtr d = deinterlaceFrame(mid.get());
        bool kept = d != nullptr;
        const int parity = (mid->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) ? 0 : 1;
        for (int p = 0; kept && p < 2; ++p) {
            const int rows = p == 0 ? mid->height : (mid->height + 1) / 2;
            for (int y = parity; y < rows; y += 2)
                kept &= std::memcmp(mid->data[p] + ptrdiff_t(mid->linesize[p]) * y,
                                    d->data[p] + ptrdiff_t(d->linesize[p]) * y, size_t(mid->width)) == 0;
        }
        std::printf("  deinterlace %s: first field %s\n", av_get_pix_fmt_name(AVPixelFormat(mid->format)),
                    kept ? "kept" : "ALTERED");
        if (!kept)
            ++failures;
    }

    std::printf("  %d frames checked: %d bit exact, %d failed", checked, exact, failures);
    if (exact + failures < checked)
        std::printf(", others within %.1f dB (neighbour frames at most %.1f dB)", worstPsnr, bestNeighbour);
    std::printf(" (decoder: %s)\n", qPrintable(decoder.decoderName()));
    return failures == 0 ? 0 : 1;
}
