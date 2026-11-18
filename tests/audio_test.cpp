// Verifies that AudioDecoder starts exactly at any requested time.
//
// usage: audio_test FILE
//
// For every audio stream: decodes from the start as a reference, then seeks
// to 40 times spread over the file (between and across frames, across
// discontinuities) and checks that the first chunk starts at that time and
// that its first 4096 samples equal the reference at the same position.

#include "media/AudioDecoder.h"
#include "media/IndexBuilder.h"
#include "media/MediaFile.h"

#include <cmath>
#include <cstdio>
#include <map>

extern "C" {
#include <libavutil/log.h>
}

using namespace adscutter;

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
        std::fprintf(stderr, "FAIL open: %s\n", qPrintable(error));
        return 1;
    }
    std::vector<StreamInfo> streams = file.streams();
    FormatContextPtr fmt = file.takeFormatContext();
    std::shared_ptr<const PacketIndex> index(buildIndex(fmt.get(), {}, &error).release());
    fmt.reset();

    constexpr int rate = 48000, channels = 2;
    int failures = 0;
    for (const StreamInfo &info : streams) {
        if (info.type != AVMEDIA_TYPE_AUDIO)
            continue;

        // Reference: everything from the start, sample positions by time.
        AudioDecoder ref;
        if (!ref.open(path, info.index, index, rate, channels, &error)) {
            std::fprintf(stderr, "FAIL stream %d: %s\n", info.index, qPrintable(error));
            ++failures;
            continue;
        }
        std::map<int64_t, std::vector<float>> chunks; // start time -> samples
        std::vector<float> samples;
        int64_t start;
        while (ref.read(samples, &start, &error))
            chunks[start] = samples;
        // Sample at time t, from the chunk that contains it.
        auto referenceAt = [&](int64_t t, size_t n, std::vector<float> &out) {
            out.clear();
            auto it = chunks.upper_bound(t);
            if (it == chunks.begin())
                return false;
            --it;
            int64_t offset = av_rescale(t - it->first, rate, AV_TIME_BASE) * channels;
            while (out.size() < n && it != chunks.end()) {
                for (size_t i = size_t(offset); i < it->second.size() && out.size() < n; ++i)
                    out.push_back(it->second[i]);
                offset = 0;
                ++it;
            }
            return out.size() == n;
        };

        const PacketIndex::Stream &s = *index->stream(info.index);
        const int64_t first = index->unifiedPts(s, s.displayOrder.front());
        const int64_t last = index->unifiedPts(s, s.displayOrder.back());
        AudioDecoder dec;
        dec.open(path, info.index, index, rate, channels, &error);
        int checked = 0, bad = 0;
        double worst = 0;
        for (int k = 0; k < 40; ++k) {
            // Spread over the stream, at odd offsets inside frames.
            const int64_t t = first + 200000 + (last - first - 400000) * k / 39 + (k * 7919) % 20000;
            if (!dec.seek(t, &error) || !dec.read(samples, &start, &error)) {
                std::fprintf(stderr, "FAIL stream %d: seek to %lld: %s\n", info.index, static_cast<long long>(t),
                             qPrintable(error));
                ++bad;
                continue;
            }
            // Gather 4096 samples (may span chunks).
            std::vector<float> got = samples;
            while (got.size() < 4096 && dec.read(samples, &start, &error))
                got.insert(got.end(), samples.begin(), samples.end());
            got.resize(std::min<size_t>(got.size(), 4096));
            std::vector<float> want;
            if (!referenceAt(t, got.size(), want)) {
                std::fprintf(stderr, "FAIL stream %d: no reference at %lld\n", info.index, static_cast<long long>(t));
                ++bad;
                continue;
            }
            double diff = 0;
            for (size_t i = 0; i < got.size(); ++i)
                diff = std::max(diff, double(std::fabs(got[i] - want[i])));
            worst = std::max(worst, diff);
            if (diff > 1e-4) {
                std::fprintf(stderr, "FAIL stream %d: at %lld us samples differ by %g\n", info.index,
                             static_cast<long long>(t), diff);
                ++bad;
            }
            ++checked;
        }
        std::printf("%s stream %d (%s): %d seeks, max difference %g\n", argv[1], info.index,
                    qPrintable(info.codecName), checked, worst);
        failures += bad;
    }
    return failures == 0 ? 0 : 1;
}
