// Opens a file, prints its streams and decodes the first video frame.
//
// usage: probe_test FILE WIDTH HEIGHT ASPECT [--expect-unexportable CODEC]
//
// Exits non-zero if the file cannot be opened, the first frame does not have
// the expected coded size / display aspect, or (with --expect-unexportable)
// the named codec is not flagged as unexportable.

#include "media/FrameDecoder.h"
#include "media/MediaFile.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include <libavutil/log.h>
}

using namespace adscutter;

int main(int argc, char **argv)
{
    if (argc < 5) {
        std::fprintf(stderr, "usage: %s FILE WIDTH HEIGHT ASPECT [--expect-unexportable CODEC]\n", argv[0]);
        return 2;
    }
    av_log_set_level(AV_LOG_FATAL);

    const QString path = QString::fromLocal8Bit(argv[1]);
    const int expectW = std::atoi(argv[2]);
    const int expectH = std::atoi(argv[3]);
    const double expectAspect = std::atof(argv[4]);
    const char *unexportable = (argc >= 7 && std::strcmp(argv[5], "--expect-unexportable") == 0) ? argv[6] : nullptr;

    MediaFile file;
    QString error;
    if (!file.open(path, &error)) {
        std::fprintf(stderr, "FAIL open: %s\n", qPrintable(error));
        return 1;
    }
    std::printf("%s: %s, %.2f s\n", argv[1], qPrintable(file.formatName()), file.duration());
    for (const StreamInfo &s : file.streams()) {
        std::printf("  #%d %-8s %-4s %s [%s] %s%s\n", s.index, qPrintable(s.typeName()),
                    qPrintable(s.language), qPrintable(s.codecName), qPrintable(s.details()),
                    s.mkvCompatible ? "mkv ok" : "not exportable",
                    s.exportNote.isEmpty() ? "" : qPrintable(QStringLiteral(" — ") + s.exportNote));
    }

    int failures = 0;
    if (unexportable) {
        bool found = false;
        for (const StreamInfo &s : file.streams()) {
            if (std::strcmp(avcodec_get_name(s.codecId), unexportable) == 0) {
                found = true;
                if (s.mkvCompatible) {
                    std::fprintf(stderr, "FAIL: %s stream #%d should be flagged unexportable\n", unexportable, s.index);
                    ++failures;
                }
            }
        }
        if (!found) {
            std::fprintf(stderr, "FAIL: no %s stream found\n", unexportable);
            ++failures;
        }
    }

    if (file.bestVideoStream() < 0) {
        std::fprintf(stderr, "FAIL: no video stream\n");
        return 1;
    }

    FrameDecoder decoder;
    if (!decoder.open(file, file.bestVideoStream(), &error)) {
        std::fprintf(stderr, "FAIL decoder: %s\n", qPrintable(error));
        return 1;
    }
    const auto frame = decoder.decodeNextFrame(&error);
    if (!frame) {
        std::fprintf(stderr, "FAIL decode: %s\n", qPrintable(error));
        return 1;
    }
    std::printf("  first frame: %dx%d aspect %.4f pts %lld%s%s\n", frame->image.width(),
                frame->image.height(), frame->displayAspect, static_cast<long long>(frame->pts),
                frame->keyframe ? " key" : "", frame->interlaced ? " interlaced" : "");

    if (frame->image.width() != expectW || frame->image.height() != expectH) {
        std::fprintf(stderr, "FAIL: expected %dx%d\n", expectW, expectH);
        ++failures;
    }
    if (std::abs(frame->displayAspect - expectAspect) > 0.01) {
        std::fprintf(stderr, "FAIL: expected aspect %.4f\n", expectAspect);
        ++failures;
    }
    // A blank (all black) image would mean the conversion went wrong:
    // testsrc2 frames are colourful everywhere.
    const QRgb px = frame->image.pixel(frame->image.width() / 4, frame->image.height() / 4);
    if (qRed(px) + qGreen(px) + qBlue(px) == 0) {
        std::fprintf(stderr, "FAIL: decoded image looks empty\n");
        ++failures;
    }
    return failures == 0 ? 0 : 1;
}
