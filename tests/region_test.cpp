// Region change detection on the broadcast-like recording (see
// make_broadcast_like.py): "MEDIASET" is shown under the logo during frames
// 500-1223, "PRIMA" otherwise, both semi-transparent; the "5 HD" logo is
// always shown.
//
// usage: region_test broadcast_like.mkv

#include "analysis/RegionJob.h"
#include "media/IndexBuilder.h"
#include "media/MediaFile.h"
#include "media/RandomAccessDecoder.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

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

void print(const char *name, const RegionResult &r)
{
    std::printf("%-10s groups %.2f / %.2f, changes:", name, r.hiddenMean, r.shownMean);
    for (uint32_t c : r.changes)
        std::printf(" %u", c);
    std::printf("; visible:");
    for (const auto &[a, b] : r.visible)
        std::printf(" %u-%u", a, b);
    std::printf("\n");
}

bool near(uint32_t value, uint32_t expected, uint32_t tolerance = 1)
{
    return uint32_t(std::abs(int64_t(value) - int64_t(expected))) <= tolerance;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s broadcast_like.mkv\n", argv[0]);
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
    const int video = file.bestVideoStream();
    FormatContextPtr fmt = file.takeFormatContext();
    std::shared_ptr<const PacketIndex> index(buildIndex(fmt.get(), {}, &error).release());
    fmt.reset();

    RandomAccessDecoder decoder;
    decoder.open(path, video, index, &error);
    // References as the UI takes them: the chosen frame and others around it
    // (one per second, up to 4 s away), within the same state here.
    std::vector<FramePtr> breakFrames, programmeFrames;
    for (uint32_t f : {600u, 650u, 700u, 750u, 800u})
        breakFrames.push_back(decoder.frameAt(f, &error));
    for (uint32_t f : {50u, 100u, 150u, 200u, 250u, 300u, 350u, 400u, 450u})
        programmeFrames.push_back(decoder.frameAt(f, &error));
    auto raw = [](const std::vector<FramePtr> &frames) {
        std::vector<const AVFrame *> out;
        for (const FramePtr &f : frames)
            out.push_back(f.get());
        return out;
    };
    const auto inBreak = raw(breakFrames), inProgramme = raw(programmeFrames);

    std::vector<WatchRegion> regions(3);
    regions[0].name = QStringLiteral("MEDIASET");
    regions[0].rect = QRect(1045, 612, 200, 44);
    regions[0].kind = WatchRegion::Kind::VisibleInAds;
    regions[1].name = QStringLiteral("PRIMA");
    regions[1].rect = QRect(1045, 612, 200, 44);
    regions[1].kind = WatchRegion::Kind::VisibleInProgramme;
    regions[2].name = QStringLiteral("5 HD");
    regions[2].rect = QRect(1060, 545, 170, 70);
    regions[2].kind = WatchRegion::Kind::VisibleInProgramme;
    if (!captureTemplate(inBreak, regions[0]) || !captureTemplate(inProgramme, regions[1])
        || !captureTemplate(inProgramme, regions[2]))
        fail("templates");

    // An empty corner has nothing to take a reference from.
    WatchRegion empty;
    empty.rect = QRect(10, 300, 100, 40);
    FramePtr black = decoder.frameAt(1218, &error);
    if (captureTemplate(black.get(), empty))
        fail("a region on a black frame should be refused");

    // Presets: JSON round trip.
    const auto restored = regionsFromJson(regionsToJson(regions));
    if (restored.size() != 3 || restored[1].name != regions[1].name || restored[1].rect != regions[1].rect
        || restored[1].kind != regions[1].kind || restored[1].templateEdges != regions[1].templateEdges)
        fail("JSON round trip");

    RegionScanSettings settings;
    settings.minRunUs = 5'000'000; // the test break lasts 14 s
    const auto results = scanRegions(path, index, video, restored, {}, &error, settings);
    if (results.size() != 3) {
        fail("scan: %s", qPrintable(error));
        return 1;
    }
    print("MEDIASET", results[0]);
    print("PRIMA", results[1]);
    print("5 HD", results[2]);

    if (results[0].changes.size() != 2 || !near(results[0].changes[0], 500) || !near(results[0].changes[1], 1224))
        fail("MEDIASET should change at 500 and 1224");
    if (results[0].visible.size() != 1)
        fail("MEDIASET should be visible once, during the break");
    if (results[1].changes.size() != 2 || !near(results[1].changes[0], 500) || !near(results[1].changes[1], 1224)
        || results[1].visible.size() != 2)
        fail("PRIMA should be visible before and after the break");
    if (!results[2].changes.empty() || results[2].visible.size() != 1 || results[2].visible[0].first != 0)
        fail("the logo is always shown: no changes");

    if (failures == 0)
        std::printf("OK\n");
    return failures == 0 ? 0 : 1;
}
