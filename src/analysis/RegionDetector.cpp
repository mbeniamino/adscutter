#include "analysis/RegionDetector.h"
#include "media/MediaFile.h"
#include "media/RandomAccessDecoder.h"

extern "C" {
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cstdlib>

namespace adscutter {

namespace {

constexpr int kEdgeThreshold = 32; // |dx| + |dy| on 8-bit luma, half resolution
constexpr int kMinTemplateEdges = 20;

struct Limits {
    double shown = 2, hidden = -1; // shown at or above, hidden at or below
};

// Classification of one frame: 1 shown, 0 not shown, -1 undecided.
int classify(double score, const Limits &l)
{
    if (score < 0)
        return -1; // the frame does not tell
    if (score >= l.shown)
        return 1;
    if (score <= l.hidden)
        return 0;
    return -1;
}

// Splits scores into two groups maximizing the variance between them.
Limits splitScores(std::vector<double> scores, const RegionScanSettings &s, double *lowMean, double *highMean)
{
    Limits l;
    if (scores.empty())
        return l;
    std::sort(scores.begin(), scores.end());
    const size_t n = scores.size();
    std::vector<double> prefix(n + 1, 0.0);
    for (size_t i = 0; i < n; ++i)
        prefix[i + 1] = prefix[i] + scores[i];
    double best = -1;
    size_t split = 0;
    for (size_t k = 1; k < n; ++k) { // low group: [0, k)
        if (scores[k] == scores[k - 1])
            continue;
        const double w0 = double(k) / n, w1 = 1 - w0;
        const double m0 = prefix[k] / k, m1 = (prefix[n] - prefix[k]) / (n - k);
        const double between = w0 * w1 * (m1 - m0) * (m1 - m0);
        if (between > best) {
            best = between;
            split = k;
        }
    }
    const double mean = prefix[n] / n;
    if (split == 0) {
        *lowMean = *highMean = mean;
    } else {
        *lowMean = prefix[split] / split;
        *highMean = (prefix[n] - prefix[split]) / (n - split);
    }
    if (*highMean - *lowMean < s.minSeparation) {
        // One state only: shown if it matches the reference well.
        if (mean >= 0.5)
            l.shown = -1;
        else
            l.hidden = 2;
        return l;
    }
    const double middle = (*lowMean + *highMean) / 2, margin = s.hysteresis * (*highMean - *lowMean);
    l.shown = middle + margin;
    l.hidden = middle - margin;
    return l;
}

} // namespace

WatchRegion WatchRegion::scaledTo(QSize size) const
{
    if (size == pictureSize || pictureSize.isEmpty())
        return *this;
    WatchRegion r = *this;
    const double sx = double(size.width()) / pictureSize.width();
    const double sy = double(size.height()) / pictureSize.height();
    r.rect = QRect(int(rect.x() * sx), int(rect.y() * sy), std::max(2, int(rect.width() * sx)),
                   std::max(2, int(rect.height() * sy)));
    r.pictureSize = size;
    // Resample the template to the new half resolution size (nearest).
    const QSize t((r.rect.width() + 1) / 2, (r.rect.height() + 1) / 2);
    r.templateEdges.assign(size_t(t.width()) * size_t(t.height()), 0);
    for (int y = 0; y < t.height(); ++y)
        for (int x = 0; x < t.width(); ++x) {
            const int ox = std::min(templateSize.width() - 1, x * templateSize.width() / t.width());
            const int oy = std::min(templateSize.height() - 1, y * templateSize.height() / t.height());
            r.templateEdges[size_t(y * t.width() + x)] = templateEdges[size_t(oy * templateSize.width() + ox)];
        }
    r.templateSize = t;
    return r;
}

EdgeMap regionEdges(const AVFrame *frame, const QRect &rect)
{
    EdgeMap map;
    const QRect r = rect.intersected(QRect(0, 0, frame->width, frame->height));
    if (r.isEmpty())
        return map;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(AVPixelFormat(frame->format));
    const int depth = desc ? desc->comp[0].depth : 8;
    map.width = (r.width() + 1) / 2;
    map.height = (r.height() + 1) / 2;

    std::vector<int> luma(size_t(map.width) * size_t(map.height));
    for (int j = 0; j < map.height; ++j) {
        const uint8_t *row = frame->data[0] + ptrdiff_t(frame->linesize[0]) * (r.top() + 2 * j);
        for (int i = 0; i < map.width; ++i) {
            const int x = r.left() + 2 * i;
            luma[size_t(j * map.width + i)] =
                depth > 8 ? reinterpret_cast<const uint16_t *>(row)[x] >> (depth - 8) : row[x];
        }
    }
    map.edges.assign(luma.size(), 0);
    for (int j = 1; j + 1 < map.height; ++j) {
        for (int i = 1; i + 1 < map.width; ++i) {
            const auto at = [&](int x, int y) { return luma[size_t(y * map.width + x)]; };
            const int g = std::abs(at(i + 1, j) - at(i - 1, j)) + std::abs(at(i, j + 1) - at(i, j - 1));
            map.edges[size_t(j * map.width + i)] = g > kEdgeThreshold;
        }
    }
    return map;
}

bool captureTemplate(const std::vector<const AVFrame *> &frames, WatchRegion &region)
{
    if (frames.empty() || !frames.front())
        return false;
    std::vector<int> votes;
    EdgeMap first;
    int used = 0;
    for (const AVFrame *frame : frames) {
        if (!frame)
            continue;
        const EdgeMap map = regionEdges(frame, region.rect);
        if (used == 0) {
            first = map;
            votes.assign(map.edges.size(), 0);
        } else if (map.width != first.width || map.height != first.height) {
            continue;
        }
        for (size_t i = 0; i < map.edges.size(); ++i)
            votes[i] += map.edges[i];
        ++used;
    }
    if (used == 0)
        return false;
    // Edges in at least 75% of the frames.
    std::vector<uint8_t> stable(votes.size());
    for (size_t i = 0; i < votes.size(); ++i)
        stable[i] = votes[i] * 4 >= used * 3;
    if (std::count(stable.begin(), stable.end(), uint8_t(1)) < kMinTemplateEdges)
        return false;
    region.pictureSize = QSize(frames.front()->width, frames.front()->height);
    region.templateSize = QSize(first.width, first.height);
    region.templateEdges = std::move(stable);
    return true;
}

double matchScore(const WatchRegion &region, const EdgeMap &frame, const std::vector<uint8_t> *overlayMask)
{
    if (frame.width != region.templateSize.width() || frame.height != region.templateSize.height())
        return 0.0;
    const auto frameEdges = std::count(frame.edges.begin(), frame.edges.end(), uint8_t(1));
    const auto templateEdges = std::count(region.templateEdges.begin(), region.templateEdges.end(), uint8_t(1));
    if (frameEdges * 4 < templateEdges)
        return -1.0; // too flat to tell (bright or dark background washing the overlay out)

    // Recall: the reference's edges found in the frame. Precision: the
    // frame's edges explained by the reference. Both within one pixel. A
    // text replaced by another in the same place ("PRIMA" -> "MEDIASET")
    // keeps a high recall in one direction; precision catches it.
    auto nearEdge = [](const std::vector<uint8_t> &edges, int w, int h, int x, int y) {
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int xx = x + dx, yy = y + dy;
                if (xx >= 0 && yy >= 0 && xx < w && yy < h && edges[size_t(yy * w + xx)])
                    return true;
            }
        return false;
    };
    const bool masked = overlayMask && overlayMask->size() == frame.edges.size();
    int recalled = 0, explained = 0, counted = 0;
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            const size_t i = size_t(y * frame.width + x);
            if (region.templateEdges[i] && nearEdge(frame.edges, frame.width, frame.height, x, y))
                ++recalled;
            if (frame.edges[i] && (!masked || (*overlayMask)[i])) {
                ++counted;
                explained += nearEdge(region.templateEdges, frame.width, frame.height, x, y);
            }
        }
    }
    const double recall = double(recalled) / double(templateEdges);
    const double precision = counted ? double(explained) / double(counted) : 1.0;
    return std::min(recall, precision);
}

std::vector<RegionResult> scanRegions(const QString &path, std::shared_ptr<const PacketIndex> index, int videoStream,
                                      const std::vector<WatchRegion> &inputRegions,
                                      const std::function<bool(double)> &progress, QString *error,
                                      const RegionScanSettings &settings)
{
    auto fail = [&](const QString &why) {
        if (error)
            *error = why;
        return std::vector<RegionResult>{};
    };
    const PacketIndex::Stream *video = index->stream(videoStream);
    if (!video || video->displayOrder.empty())
        return fail(QStringLiteral("No video to scan"));
    if (inputRegions.empty())
        return fail(QStringLiteral("No regions to look for"));

    FormatContextPtr fmt = openFormatContext(path, error);
    if (!fmt)
        return {};
    if (fmt->nb_streams != index->streams().size())
        return fail(QStringLiteral("The file does not match its index; rebuild the index"));
    for (unsigned i = 0; i < fmt->nb_streams; ++i)
        if (int(i) != videoStream)
            fmt->streams[i]->discard = AVDISCARD_ALL;
    const AVStream *st = fmt->streams[videoStream];
    std::vector<WatchRegion> regions;
    for (const WatchRegion &r : inputRegions)
        regions.push_back(r.scaledTo(QSize(st->codecpar->width, st->codecpar->height)));

    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    CodecContextPtr dec(codec ? avcodec_alloc_context3(codec) : nullptr);
    if (!dec || avcodec_parameters_to_context(dec.get(), st->codecpar) < 0)
        return fail(QStringLiteral("Cannot open the video decoder"));
    dec->pkt_timebase = st->time_base;
    dec->thread_count = 0;
    dec->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
    dec->skip_frame = AVDISCARD_NONKEY; // only the cut points are sent anyway
    if (avcodec_open2(dec.get(), codec, nullptr) < 0)
        return fail(QStringLiteral("Cannot open the video decoder"));

    // --- Coarse pass: every cut point frame -------------------------------------
    struct Sample {
        uint32_t pos;
        double score;
        int state = -1;
        std::vector<bool> edges; // kept to rebuild the reference afterwards
    };
    std::vector<std::vector<Sample>> samples(regions.size());
    PacketPtr pkt(av_packet_alloc());
    FramePtr frame(av_frame_alloc());
    auto drain = [&] {
        while (avcodec_receive_frame(dec.get(), frame.get()) >= 0) {
            const auto packet = uintptr_t(frame->opaque);
            const int32_t pos = packet >= 1 && packet <= video->packets.size() ? video->displayPosOf[packet - 1] : -1;
            if (pos >= 0) {
                for (size_t r = 0; r < regions.size(); ++r) {
                    EdgeMap map = regionEdges(frame.get(), regions[r].rect);
                    const double score = matchScore(regions[r], map);
                    samples[r].push_back({uint32_t(pos), score, -1,
                                          std::vector<bool>(map.edges.begin(), map.edges.end())});
                }
            }
            av_frame_unref(frame.get());
        }
    };
    uint32_t counter = 0;
    uint64_t count = 0;
    const int64_t fileSize = avio_size(fmt->pb);
    while (av_read_frame(fmt.get(), pkt.get()) >= 0) {
        if (pkt->stream_index != videoStream) {
            av_packet_unref(pkt.get());
            continue;
        }
        const uint32_t i = counter++;
        if (i < video->packets.size() && video->isCutPoint(video->packets[i])) {
            pkt->opaque = reinterpret_cast<void *>(uintptr_t(i) + 1);
            avcodec_send_packet(dec.get(), pkt.get());
            drain();
        }
        av_packet_unref(pkt.get());
        if (++count % 256 == 0 && progress && fileSize > 0
            && !progress(0.8 * std::min(1.0, double(avio_tell(fmt->pb)) / double(fileSize)))) {
            if (error)
                error->clear();
            return {};
        }
    }
    avcodec_send_packet(dec.get(), nullptr);
    drain();

    // --- Confirmed changes, then frame exact refinement ---------------------------
    RandomAccessDecoder exact;
    if (!exact.open(path, videoStream, index, error))
        return {};
    std::vector<RegionResult> results(regions.size());
    const auto frames = uint32_t(video->displayOrder.size());
    for (size_t r = 0; r < regions.size(); ++r) {
        auto &s = samples[r];
        std::sort(s.begin(), s.end(), [](const Sample &a, const Sample &b) { return a.pos < b.pos; });
        auto split = [&] {
            std::vector<double> scores;
            for (const Sample &x : s)
                if (x.score >= 0)
                    scores.push_back(x.score);
            return splitScores(scores, settings, &results[r].hiddenMean, &results[r].shownMean);
        };
        // Where overlays live: pixels that are edges in at least a quarter of
        // all samples (both states of a changing text qualify; a particular
        // background scene does not, in a recording of any length). The
        // reference itself is always part of it.
        const size_t pixels = regions[r].templateEdges.size();
        std::vector<uint8_t> mask(pixels, 0);
        {
            std::vector<int> count(pixels, 0);
            int n = 0;
            for (const Sample &x : s) {
                if (x.edges.size() != pixels)
                    continue;
                for (size_t i = 0; i < pixels; ++i)
                    count[i] += x.edges[i];
                ++n;
            }
            for (size_t i = 0; i < pixels; ++i)
                mask[i] = regions[r].templateEdges[i] || (n > 0 && count[i] * 4 >= n);
        }
        auto rescore = [&] {
            for (Sample &x : s) {
                if (x.edges.size() != pixels)
                    continue;
                EdgeMap map;
                map.width = regions[r].templateSize.width();
                map.height = regions[r].templateSize.height();
                map.edges.assign(x.edges.begin(), x.edges.end());
                x.score = matchScore(regions[r], map, &mask);
            }
        };
        rescore();
        Limits limits = split();

        // Rebuild the reference from the samples clearly showing the
        // overlay, all over the recording: background edges that happened to
        // stay still around the chosen frame drop out. Then score again.
        {
            std::vector<int> votes(pixels, 0);
            int shown = 0;
            for (const Sample &x : s) {
                if (x.score < limits.shown || x.edges.size() != pixels)
                    continue;
                for (size_t i = 0; i < pixels; ++i)
                    votes[i] += x.edges[i];
                ++shown;
            }
            if (shown >= 8) {
                std::vector<uint8_t> refined(pixels);
                for (size_t i = 0; i < pixels; ++i)
                    refined[i] = votes[i] * 10 >= shown * 6; // in 60% of them
                if (std::count(refined.begin(), refined.end(), uint8_t(1)) >= kMinTemplateEdges) {
                    regions[r].templateEdges = std::move(refined);
                    rescore();
                    limits = split();
                }
            }
        }
        for (Sample &x : s) {
            x.state = classify(x.score, limits);
            std::vector<bool>().swap(x.edges);
        }
        int state = -1;
        size_t lastOld = 0;
        std::vector<std::pair<uint32_t, uint32_t>> transitions; // (last sample in old state, first in new)
        for (size_t k = 0; k < s.size(); ++k) {
            if (s[k].state < 0)
                continue;
            if (state < 0) {
                state = s[k].state;
                lastOld = k;
                continue;
            }
            if (s[k].state == state) {
                lastOld = k;
                continue;
            }
            // Confirm: the next samples (ignoring undecided ones) keep the new state.
            int confirmed = 0;
            for (size_t m = k; m < s.size() && confirmed < settings.confirmSamples; ++m) {
                if (s[m].state < 0)
                    continue;
                if (s[m].state != s[k].state)
                    break;
                ++confirmed;
            }
            if (confirmed < settings.confirmSamples)
                continue; // a blip
            transitions.emplace_back(s[lastOld].pos, s[k].pos);
            state = s[k].state;
            lastOld = k;
        }
        const int initial = [&] {
            for (const Sample &x : s)
                if (x.state >= 0)
                    return x.state;
            return 0;
        }();

        // Exact frame: one after the last frame still in the old state.
        int current = initial;
        for (const auto &[from, to] : transitions) {
            uint32_t change = to;
            for (uint32_t f = to; f-- > from;) {
                QString ignored;
                FramePtr fr = exact.frameAt(f, &ignored);
                if (!fr)
                    continue;
                if (classify(matchScore(regions[r], regionEdges(fr.get(), regions[r].rect), &mask), limits) == current) {
                    change = f + 1;
                    break;
                }
                change = f;
            }
            results[r].changes.push_back(change);
            current = 1 - current;
        }

        // Runs of each state; short runs are absorbed by their neighbours.
        struct Run {
            bool visible;
            uint32_t first, last;
        };
        std::vector<Run> runs;
        {
            bool visible = initial == 1;
            uint32_t start = 0;
            for (uint32_t change : results[r].changes) {
                if (change > start)
                    runs.push_back({visible, start, change - 1});
                visible = !visible;
                start = change;
            }
            if (start < frames)
                runs.push_back({visible, start, frames - 1});
        }
        auto duration = [&](const Run &x) {
            return index->frameTime(*video, x.last) - index->frameTime(*video, x.first);
        };
        for (bool merged = true; merged;) {
            merged = false;
            // The shortest inner run first, so that real states survive.
            size_t shortest = 0;
            int64_t shortestUs = INT64_MAX;
            for (size_t k = 1; k + 1 < runs.size(); ++k) {
                if (duration(runs[k]) < shortestUs) {
                    shortestUs = duration(runs[k]);
                    shortest = k;
                }
            }
            if (shortest > 0 && shortestUs < settings.minRunUs) {
                runs[shortest - 1].last = runs[shortest + 1].last;
                runs.erase(runs.begin() + ptrdiff_t(shortest), runs.begin() + ptrdiff_t(shortest) + 2);
                merged = true;
            }
        }
        results[r].changes.clear();
        for (size_t k = 0; k < runs.size(); ++k) {
            if (k > 0)
                results[r].changes.push_back(runs[k].first);
            if (runs[k].visible)
                results[r].visible.emplace_back(runs[k].first, runs[k].last);
        }

        if (progress && !progress(0.8 + 0.2 * double(r + 1) / double(regions.size()))) {
            if (error)
                error->clear();
            return {};
        }
    }
    return results;
}

} // namespace adscutter
