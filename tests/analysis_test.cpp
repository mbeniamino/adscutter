// Analyses the broadcast-like test recording (see make_broadcast_like.py)
// and checks what is found against how it was built.
//
// usage: analysis_test broadcast_like.mkv

#include "analysis/Analyzer.h"
#include "media/IndexBuilder.h"
#include "media/MediaFile.h"

#include <QTemporaryDir>
#include <cstdarg>
#include <cstring>
#include <cstdio>

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

bool within(uint32_t value, uint32_t low, uint32_t high)
{
    return value >= low && value <= high;
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
    int video = file.bestVideoStream(), audio = -1;
    for (const StreamInfo &s : file.streams())
        if (s.type == AVMEDIA_TYPE_AUDIO && audio < 0)
            audio = s.index;
    FormatContextPtr fmt = file.takeFormatContext();
    std::shared_ptr<const PacketIndex> index(buildIndex(fmt.get(), {}, &error).release());
    fmt.reset();

    auto data = analyzeRecording(path, *index, video, audio, {}, &error);
    if (!data) {
        fail("analysis: %s", qPrintable(error));
        return 1;
    }
    if (!data->isComplete())
        fail("not every frame was analysed");

    const auto candidates = findBreakCandidates(*data, *index);
    const auto silences = findSilences(*data);
    const auto breaks = findBreaks(candidates, *index, video);
    std::printf("%zu candidates, %zu silences, %zu breaks\n", candidates.size(), silences.size(), breaks.size());
    for (const auto &c : candidates)
        std::printf("  dark %u-%u%s\n", c.first, c.last, c.silent ? " + silence" : "");
    for (const auto &[a, b] : silences)
        std::printf("  silence %.3f-%.3f s\n", a / 1e6, b / 1e6);
    for (const auto &b : breaks)
        std::printf("  break %u-%u (%u candidates)\n", b.first, b.last, b.candidates);

    // The programme's fade (not silent: the sound only dips), and the two
    // separators (black and silent).
    if (candidates.size() != 3) {
        fail("expected 3 candidates");
    } else {
        if (!within(candidates[0].first, 485, 499) || !within(candidates[0].last, 499, 499))
            fail("fade to black not found at 485-499 .. 499");
        if (!candidates[0].fade || candidates[0].silent)
            fail("the programme fade should be a fade, not silent");
        if (!within(candidates[1].first, 899, 901) || !within(candidates[1].last, 910, 912) || !candidates[1].silent)
            fail("first separator not found at 900-911");
        if (!within(candidates[2].first, 1211, 1213) || !within(candidates[2].last, 1222, 1224) || !candidates[2].silent)
            fail("second separator not found at 1212-1223");
    }
    // One break: from the fade (before the ident and the first spot) to the
    // last separator.
    if (breaks.size() != 1 || breaks[0].candidates != 2 || !breaks[0].startsWithFade
        || breaks.front().first != candidates.front().first)
        fail("expected one break starting at the fade");

    // Programme, ident and spots are loud; the separators are silent (the
    // ident's last 0.2 s are just under the minimum silence length).
    if (silences.size() != 2)
        fail("expected 2 silences");
    const PacketIndex::Stream &v = *index->stream(video);
    for (uint32_t pos : {100u, 300u, 550u, 750u, 1050u, 1400u, 1700u}) {
        const uint8_t l = data->loudnessAt(index->frameTime(v, pos));
        if (l == AnalysisData::kNoAudio || l > 30)
            fail("frame %u should be loud (got -%u dBFS)", pos, l);
    }

    // Cache round trip.
    QTemporaryDir dir;
    const QString cache = AnalysisData::cachePath(path, dir.path());
    if (!data->save(cache, FileKey::of(path), &error)) {
        fail("save: %s", qPrintable(error));
    } else {
        auto loaded = AnalysisData::load(cache, FileKey::of(path), &error);
        if (!loaded || loaded->loudness != data->loudness || loaded->frames.size() != data->frames.size()
            || std::memcmp(loaded->frames.data(), data->frames.data(), data->frames.size() * sizeof(FrameFeatures)) != 0)
            fail("cache round trip");
        FileKey stale = FileKey::of(path);
        stale.size += 1;
        if (AnalysisData::load(cache, stale, &error))
            fail("stale analysis accepted");
    }

    if (failures == 0)
        std::printf("OK\n");
    return failures == 0 ? 0 : 1;
}
