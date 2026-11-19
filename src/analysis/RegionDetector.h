#pragma once

#include "media/FFmpegUtil.h"
#include "media/PacketIndex.h"

#include <QRect>
#include <QString>
#include <functional>
#include <vector>

namespace adscutter {

// A rectangle of the picture holding a text or logo that is shown during the
// programme and not during ads, or the other way round.
struct WatchRegion {
    enum class Kind { VisibleInProgramme, VisibleInAds };

    QString name;
    QRect rect;              // in coded picture pixels
    Kind kind = Kind::VisibleInAds;
    QSize pictureSize;       // size of the picture the rectangle was drawn on
    // Edge map of the region on the reference frame, half resolution, one
    // byte per pixel (0/1), templateSize.width() x templateSize.height().
    QSize templateSize;
    std::vector<uint8_t> templateEdges;

    bool isValid() const { return !rect.isEmpty() && !templateEdges.empty(); }
    // The region for a picture of another size (scaled proportionally).
    WatchRegion scaledTo(QSize size) const;
};

// Edge map of a region of a decoded frame (luma), half resolution:
// sampling every other line keeps interlaced combing out of it.
struct EdgeMap {
    int width = 0, height = 0;
    std::vector<uint8_t> edges;
};
EdgeMap regionEdges(const AVFrame *frame, const QRect &rect);

// Takes the region's reference from frames showing the text or logo (a
// few seconds around the chosen one): only edges present in most of them
// are kept, so the moving picture behind a semi-transparent overlay drops
// out. False if no stable edges remain.
bool captureTemplate(const std::vector<const AVFrame *> &frames, WatchRegion &region);
inline bool captureTemplate(const AVFrame *frame, WatchRegion &region)
{
    return captureTemplate(std::vector<const AVFrame *>{frame}, region);
}

// How well the frame shows the reference (0..1): the lower of the share of
// the reference's edges found in the frame and the share of the frame's
// edges explained by the reference (within one pixel). Negative when the
// frame says nothing either way: the region has hardly any edges (e.g. a
// semi-transparent white text over a bright picture is invisible).
// `overlayMask` (optional, same size): pixels where overlays show up in the
// recording; precision only counts frame edges there, so a background
// with edges of its own does not count against the reference.
double matchScore(const WatchRegion &region, const EdgeMap &frame, const std::vector<uint8_t> *overlayMask = nullptr);

// Scores are not compared with fixed limits: a text that changes into
// another one (e.g. "PRIMA" / "MEDIASET" under a logo) keeps many edges in
// common, so "not shown" can still score 0.7. Instead the scores of the
// coarse pass are split into two groups (Otsu), and the limits are placed
// between them.
struct RegionScanSettings {
    // Groups closer than this: the region never changes. A semi-transparent
    // overlay's edges vary with the brightness behind it (the outline only
    // shows over bright pictures): an unchanging logo scored 0.82 vs 0.99
    // over darker vs brighter programme parts, while real changes measured
    // gaps of 0.27 (Canale 5 "PRIMA"/"MEDIASET") to 0.38.
    double minSeparation = 0.20;
    double hysteresis = 0.25;    // share of the gap between groups left undecided on each side
    int confirmSamples = 2;      // a change seen on fewer coarse samples is ignored
    // A state lasting less than this between two runs of the other state is
    // absorbed by them (a spot where the text is hard to see, a short promo).
    int64_t minRunUs = 30'000'000;
};

struct RegionResult {
    double hiddenMean = 0, shownMean = 0; // the two groups of scores found
    std::vector<std::pair<uint32_t, uint32_t>> visible; // where shown: [first, last] inclusive
    std::vector<uint32_t> changes;                      // first frame of each new state, sorted
};

// Scans the recording for the regions: coarse pass on the cut point frames,
// then frame exact refinement around each change. progress(fraction) may
// return false to cancel (empty result, empty error).
std::vector<RegionResult> scanRegions(const QString &path, std::shared_ptr<const PacketIndex> index, int videoStream,
                                      const std::vector<WatchRegion> &regions,
                                      const std::function<bool(double)> &progress, QString *error,
                                      const RegionScanSettings &settings = {});

} // namespace adscutter
