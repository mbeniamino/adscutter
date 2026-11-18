#include "ui/Timeline.h"
#include "model/CutList.h"

#include <QMouseEvent>
#include <QPainter>
#include <QTime>
#include <QWheelEvent>
#include <cmath>
#include <tuple>

namespace adscutter {

namespace {

constexpr int kRulerHeight = 20;
constexpr int kTrackTop = 22;
constexpr int kTrackHeight = 34;
constexpr int kOverviewHeight = 10;
constexpr int kLaneHeight = 9;
constexpr int kLaneGap = 2;
constexpr double kEdgeGrabPx = 5.0;

const QColor kKept(76, 160, 92);
const QColor kRemoved(160, 64, 64, 70);
const QColor kInMark(52, 120, 220);
const QColor kOutMark(230, 140, 30);
const QColor kPlayhead(230, 40, 40);
const QColor kBreak(240, 150, 30);

QString rulerLabel(int64_t us, double intervalS)
{
    const QTime t = QTime(0, 0).addMSecs(us / 1000);
    if (intervalS >= 60)
        return t.toString(QStringLiteral("H:mm"));
    return t.toString(us >= 3'600'000'000 ? QStringLiteral("H:mm:ss") : QStringLiteral("m:ss"));
}

} // namespace

Timeline::Timeline(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setToolTip(tr("Click or drag to move · drag a segment edge to adjust it\n"
                  "Wheel: zoom · Shift+wheel: scroll · bottom strip: whole recording\n"
                  "After Analysis → Analyze Recording: orange = breaks found (marks = spot separators),\n"
                  "lanes below: darkness, loudness (blue = silence), subtitles"));
}

void Timeline::setSource(std::shared_ptr<const PacketIndex> index, int streamIndex)
{
    m_index = std::move(index);
    m_stream = m_index ? m_index->stream(streamIndex) : nullptr;
    if (m_stream && m_stream->displayOrder.empty())
        m_stream = nullptr;
    m_playhead = 0;
    m_subtitleFrames.clear();
    if (m_stream) {
        for (const PacketIndex::Stream &s : m_index->streams()) {
            if (s.type != AVMEDIA_TYPE_SUBTITLE)
                continue;
            for (uint32_t pos = 0; pos < s.displayOrder.size(); ++pos)
                m_subtitleFrames.push_back(m_index->positionAtTime(*m_stream, m_index->unifiedPts(s, s.displayOrder[pos])));
        }
        std::sort(m_subtitleFrames.begin(), m_subtitleFrames.end());
    }
    m_analysis = nullptr;
    m_candidates.clear();
    m_breaks.clear();
    updateGeometry();
    zoomFit();
}

void Timeline::clearSource()
{
    m_index.reset();
    m_stream = nullptr;
    m_drag = Drag::None;
    update();
}

uint32_t Timeline::frameCount() const
{
    return m_stream ? uint32_t(m_stream->displayOrder.size()) : 0;
}

uint32_t Timeline::frameAt(double x) const
{
    const double pos = std::floor(posAt(x));
    if (pos <= 0 || frameCount() == 0)
        return 0;
    return uint32_t(std::min<double>(pos, frameCount() - 1));
}

double Timeline::maxFramesPerPixel() const
{
    return std::max(minFramesPerPixel(), double(std::max<uint32_t>(frameCount(), 1)) / std::max(1, width()));
}

void Timeline::setView(double start, double framesPerPixel)
{
    m_framesPerPixel = std::clamp(framesPerPixel, minFramesPerPixel(), maxFramesPerPixel());
    const double visible = width() * m_framesPerPixel;
    m_viewStart = std::clamp(start, 0.0, std::max(0.0, double(frameCount()) - visible));
    m_fitted = m_framesPerPixel >= maxFramesPerPixel() * 0.999;
    update();
}

void Timeline::zoomIn()
{
    const double anchor = m_playhead + 0.5;
    const double x = xOf(anchor);
    const double fpp = m_framesPerPixel / 1.5;
    setView(anchor - x * fpp, fpp);
}

void Timeline::zoomOut()
{
    const double anchor = m_playhead + 0.5;
    const double x = xOf(anchor);
    const double fpp = m_framesPerPixel * 1.5;
    setView(anchor - x * fpp, fpp);
}

void Timeline::zoomFit()
{
    setView(0.0, maxFramesPerPixel());
}

void Timeline::ensureVisible(uint32_t pos)
{
    const double x = xOf(pos);
    if (x >= 0 && xOf(pos + 1) <= width())
        return;
    // Page so that the playhead lands at a quarter of the view.
    setView(pos - 0.25 * width() * m_framesPerPixel, m_framesPerPixel);
}

void Timeline::setPlayhead(uint32_t pos)
{
    m_playhead = pos;
    if (m_drag == Drag::None)
        ensureVisible(pos);
    update();
}

QRectF Timeline::rulerRect() const
{
    return QRectF(0, 0, width(), kRulerHeight);
}

QRectF Timeline::trackRect() const
{
    return QRectF(0, kTrackTop, width(), kTrackHeight);
}

int Timeline::laneCount() const
{
    return (m_analysis ? 2 : 0) + (m_subtitleFrames.empty() ? 0 : 1);
}

QRectF Timeline::laneRect(int lane) const
{
    return QRectF(0, kTrackTop + kTrackHeight + kLaneGap + lane * (kLaneHeight + kLaneGap), width(), kLaneHeight);
}

QRectF Timeline::overviewRect() const
{
    const int top = kTrackTop + kTrackHeight + kLaneGap + laneCount() * (kLaneHeight + kLaneGap) + 4;
    return QRectF(0, top, width(), kOverviewHeight);
}

int Timeline::totalHeight() const
{
    return int(overviewRect().bottom()) + 4;
}

void Timeline::setAnalysis(const AnalysisData *analysis, std::vector<BreakCandidate> candidates,
                           std::vector<BreakRegion> breaks)
{
    m_analysis = analysis;
    m_candidates = std::move(candidates);
    m_breaks = std::move(breaks);
    m_columnsKey = {-1, -1, -1};
    updateGeometry();
    update();
}

void Timeline::resizeEvent(QResizeEvent *)
{
    if (m_fitted)
        zoomFit();
    else
        setView(m_viewStart, m_framesPerPixel);
}

// --- Painting -------------------------------------------------------------------

void Timeline::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), palette().window());
    if (!m_stream) {
        p.setPen(palette().color(QPalette::PlaceholderText));
        p.drawText(trackRect(), Qt::AlignCenter, tr("Timeline available once the recording is indexed"));
        return;
    }
    paintRuler(p);
    paintTrack(p);
    paintLanes(p);
    paintOverview(p);
}

void Timeline::paintRuler(QPainter &p)
{
    const double fps = m_index->durationUs() > 0 ? frameCount() / (m_index->durationUs() / 1e6) : 25.0;
    const double secondsPerPixel = m_framesPerPixel / std::max(1.0, fps);
    static const double intervals[] = {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200};
    double interval = intervals[std::size(intervals) - 1];
    for (double i : intervals) {
        if (i / secondsPerPixel >= 90) {
            interval = i;
            break;
        }
    }

    const QColor text = palette().color(QPalette::WindowText);
    QColor tick = text;
    tick.setAlpha(120);
    QFont font = p.font();
    font.setPointSizeF(font.pointSizeF() * 0.85);
    p.setFont(font);

    const uint32_t first = frameAt(0);
    const uint32_t last = frameAt(width());
    const int64_t tFirst = m_index->frameTime(*m_stream, first);
    const int64_t tLast = m_index->frameTime(*m_stream, last);
    const auto intervalUs = int64_t(interval * 1e6);
    int64_t prevPos = -1;
    for (int64_t t = (tFirst / intervalUs) * intervalUs; t <= tLast + intervalUs; t += intervalUs) {
        const uint32_t pos = m_index->positionAtTime(*m_stream, t);
        if (int64_t(pos) == prevPos)
            continue;
        prevPos = pos;
        const double x = xOf(pos);
        if (x < -100 || x > width())
            continue;
        p.setPen(tick);
        p.drawLine(QPointF(x, kRulerHeight - 6), QPointF(x, kRulerHeight));
        p.setPen(text);
        p.drawText(QRectF(x + 3, 0, 120, kRulerHeight - 2), Qt::AlignLeft | Qt::AlignVCenter,
                   rulerLabel(m_index->frameTime(*m_stream, pos), interval));
    }
}

void Timeline::paintTrack(QPainter &p)
{
    const QRectF track = trackRect();
    p.fillRect(track, palette().base());
    p.fillRect(track, kRemoved);

    const double pxPerFrame = 1.0 / m_framesPerPixel;
    const uint32_t first = frameAt(0);
    const uint32_t last = frameAt(width());

    // Kept segments.
    if (m_cutList) {
        const auto &segs = m_cutList->segments();
        for (size_t i = 0; i < segs.size(); ++i) {
            const double x0 = xOf(segs[i].in), x1 = xOf(double(segs[i].out) + 1);
            if (x1 < 0 || x0 > width())
                continue;
            const QRectF r(QPointF(std::max(-1.0, x0), track.top()), QPointF(std::min(width() + 1.0, x1), track.bottom()));
            p.fillRect(r, kKept);
        }
    }

    // Breaks found by the analysis, and their candidates (spot separators).
    for (const BreakRegion &br : m_breaks) {
        const double x0 = xOf(br.first), x1 = xOf(double(br.last) + 1);
        if (x1 < 0 || x0 > width())
            continue;
        QColor c = kBreak;
        c.setAlpha(80);
        p.fillRect(QRectF(QPointF(x0, track.top()), QPointF(std::max(x1, x0 + 2), track.top() + 6)), kBreak);
        p.fillRect(QRectF(QPointF(x0, track.top()), QPointF(std::max(x1, x0 + 2), track.bottom())), c);
    }
    for (const BreakCandidate &c : m_candidates) {
        if (!c.silent)
            continue;
        const double x = xOf(c.first);
        if (x < -2 || x > width() + 2)
            continue;
        p.fillRect(QRectF(x, track.top(), std::max(1.5, xOf(double(c.last) + 1) - x), 6), kBreak.darker(160));
    }

    // Cut points, when they are far enough apart to be told apart.
    const double avgGop = m_stream->keyframes.size() > 1 ? double(frameCount()) / m_stream->keyframes.size() : 1e9;
    if (avgGop * pxPerFrame >= 4) {
        QColor c = palette().color(QPalette::WindowText);
        c.setAlpha(110);
        p.setPen(QPen(c, 1));
        auto it = std::lower_bound(m_stream->keyframes.begin(), m_stream->keyframes.end(), first);
        for (; it != m_stream->keyframes.end() && *it <= last + 1; ++it) {
            const double x = std::round(xOf(*it)) + 0.5;
            p.drawLine(QPointF(x, track.top()), QPointF(x, track.top() + 8));
        }
    }
    // Individual frames when zoomed in a lot.
    if (pxPerFrame >= 5) {
        QColor c = palette().color(QPalette::WindowText);
        c.setAlpha(50);
        p.setPen(QPen(c, 1));
        for (uint32_t f = first; f <= last + 1 && f <= frameCount(); ++f) {
            const double x = std::round(xOf(f)) + 0.5;
            p.drawLine(QPointF(x, track.bottom() - 5), QPointF(x, track.bottom()));
        }
    }

    // Segment edges (highlighted when hovered or dragged).
    if (m_cutList) {
        const auto &segs = m_cutList->segments();
        for (size_t i = 0; i < segs.size(); ++i) {
            for (const bool isIn : {true, false}) {
                const double x = isIn ? xOf(segs[i].in) : xOf(double(segs[i].out) + 1);
                if (x < -3 || x > width() + 3)
                    continue;
                const Edge &active = m_drag == Drag::Edge ? m_dragEdge : m_hoverEdge;
                const bool hot = active.segment == int(i) && active.isIn == isIn;
                p.setPen(QPen(kKept.darker(hot ? 200 : 150), hot ? 3 : 1));
                p.drawLine(QPointF(x, track.top()), QPointF(x, track.bottom()));
            }
        }

        // Pending marks.
        auto mark = [&](std::optional<uint32_t> pos, bool isIn) {
            if (!pos)
                return;
            const double x = isIn ? xOf(*pos) : xOf(double(*pos) + 1);
            const QColor c = isIn ? kInMark : kOutMark;
            p.setPen(QPen(c, 2));
            p.drawLine(QPointF(x, track.top()), QPointF(x, track.bottom()));
            const double dir = isIn ? 1 : -1;
            QPolygonF flag;
            flag << QPointF(x, track.top()) << QPointF(x + dir * 12, track.top() + 6) << QPointF(x, track.top() + 12);
            p.setBrush(c);
            p.setPen(Qt::NoPen);
            p.drawPolygon(flag);
            p.setBrush(Qt::NoBrush);
        };
        mark(m_cutList->pendingIn(), true);
        mark(m_cutList->pendingOut(), false);
    }

    // Playhead: a line, or the whole frame when frames are wide.
    const double x0 = xOf(m_playhead), x1 = xOf(double(m_playhead) + 1);
    if (x1 - x0 >= 4) {
        QColor fill = kPlayhead;
        fill.setAlpha(70);
        p.fillRect(QRectF(QPointF(x0, track.top()), QPointF(x1, track.bottom())), fill);
    }
    p.setPen(QPen(kPlayhead, 2));
    p.drawLine(QPointF(x0, 0), QPointF(x0, track.bottom()));

    p.setPen(palette().color(QPalette::Mid));
    p.setBrush(Qt::NoBrush);
    p.drawRect(track.adjusted(0, 0, -1, 0));
}

void Timeline::computeColumns()
{
    const auto key = std::make_tuple(m_viewStart, m_framesPerPixel, width());
    if (key == m_columnsKey)
        return;
    m_columnsKey = key;
    const int w = std::max(1, width());
    m_darkColumns.assign(size_t(w), 0);
    m_loudColumns.assign(size_t(w), AnalysisData::kNoAudio);
    if (!m_analysis || m_analysis->frames.size() != frameCount())
        return;
    for (int x = 0; x < w; ++x) {
        const auto from = uint32_t(std::clamp(std::floor(posAt(x)), 0.0, double(frameCount())));
        const auto to = uint32_t(std::clamp(std::ceil(posAt(x + 1)), double(from + 1), double(frameCount())));
        if (from >= frameCount())
            break;
        uint8_t dark = 0;
        for (uint32_t f = from; f < to; ++f)
            dark = std::max(dark, m_analysis->frames[f].dark);
        m_darkColumns[size_t(x)] = dark;
        // Audio: the quietest window in the column (silence must stand out).
        const int64_t t0 = m_index->frameTime(*m_stream, from);
        const int64_t t1 = to < frameCount() ? m_index->frameTime(*m_stream, to) : t0 + AnalysisData::kAudioWindowUs;
        uint8_t quietest = 0;
        bool any = false;
        for (int64_t t = t0; t < std::max(t1, t0 + 1); t += AnalysisData::kAudioWindowUs) {
            const uint8_t l = m_analysis->loudnessAt(t);
            if (l != AnalysisData::kNoAudio) {
                quietest = std::max(quietest, l);
                any = true;
            }
        }
        m_loudColumns[size_t(x)] = any ? quietest : AnalysisData::kNoAudio;
    }
}

void Timeline::paintLanes(QPainter &p)
{
    int lane = 0;
    if (m_analysis) {
        computeColumns();
        // Darkness: the darker the frames under a pixel, the darker the line.
        const QRectF dark = laneRect(lane++);
        p.fillRect(dark, palette().base());
        for (int x = 0; x < int(m_darkColumns.size()); ++x) {
            if (m_darkColumns[size_t(x)] < 64)
                continue;
            QColor c(20, 20, 20);
            c.setAlpha(m_darkColumns[size_t(x)]);
            p.fillRect(QRectF(x, dark.top(), 1, dark.height()), c);
        }
        // Loudness: a level bar; silence in blue.
        const QRectF loud = laneRect(lane++);
        p.fillRect(loud, palette().base());
        QColor level = palette().color(QPalette::WindowText);
        level.setAlpha(110);
        for (int x = 0; x < int(m_loudColumns.size()); ++x) {
            const uint8_t db = m_loudColumns[size_t(x)];
            if (db == AnalysisData::kNoAudio)
                continue;
            if (db >= 55) {
                p.fillRect(QRectF(x, loud.top(), 1, loud.height()), QColor(60, 120, 230));
            } else {
                const double h = loud.height() * std::clamp((60.0 - db) / 50.0, 0.1, 1.0);
                p.fillRect(QRectF(x, loud.bottom() - h, 1, h), level);
            }
        }
    }
    if (!m_subtitleFrames.empty()) {
        const QRectF subs = laneRect(lane++);
        p.fillRect(subs, palette().base());
        QColor c(140, 90, 200);
        auto it = std::lower_bound(m_subtitleFrames.begin(), m_subtitleFrames.end(), frameAt(0));
        for (; it != m_subtitleFrames.end() && *it <= frameAt(width()) + 1; ++it)
            p.fillRect(QRectF(std::round(xOf(*it)), subs.top(), 1.5, subs.height()), c);
    }
}

void Timeline::paintOverview(QPainter &p)
{
    const QRectF r = overviewRect();
    const double scale = r.width() / std::max<uint32_t>(frameCount(), 1);
    p.fillRect(r, palette().base());
    p.fillRect(r, kRemoved);
    if (m_cutList) {
        for (const Segment &s : m_cutList->segments())
            p.fillRect(QRectF(s.in * scale, r.top(), std::max(1.0, (double(s.out) + 1 - s.in) * scale), r.height()), kKept);
    }
    if (!m_fitted) {
        QColor c = palette().color(QPalette::Highlight);
        p.setPen(QPen(c, 1.5));
        p.drawRect(QRectF(m_viewStart * scale, r.top() - 1, std::max(3.0, width() * m_framesPerPixel * scale), r.height() + 1));
    }
    p.setPen(QPen(kPlayhead, 1.5));
    const double x = m_playhead * scale;
    p.drawLine(QPointF(x, r.top() - 1), QPointF(x, r.bottom()));
}

// --- Interaction ----------------------------------------------------------------

Timeline::Edge Timeline::edgeAt(double x, double y) const
{
    Edge best;
    if (!m_cutList || y < kTrackTop || y > kTrackTop + kTrackHeight)
        return best;
    double bestDistance = kEdgeGrabPx;
    const auto &segs = m_cutList->segments();
    for (size_t i = 0; i < segs.size(); ++i) {
        for (const bool isIn : {true, false}) {
            const double ex = isIn ? xOf(segs[i].in) : xOf(double(segs[i].out) + 1);
            const double d = std::abs(ex - x);
            if (d <= bestDistance) {
                bestDistance = d;
                best = {int(i), isIn};
            }
        }
    }
    return best;
}

void Timeline::mousePressEvent(QMouseEvent *event)
{
    if (!m_stream || event->button() != Qt::LeftButton)
        return;
    const QPointF pt = event->position();
    if (pt.y() >= overviewRect().top() - 2) {
        m_drag = Drag::Overview;
        const double pos = pt.x() / width() * frameCount();
        setView(pos - 0.5 * width() * m_framesPerPixel, m_framesPerPixel);
        return;
    }
    const Edge edge = edgeAt(pt.x(), pt.y());
    if (edge.segment >= 0) {
        m_drag = Drag::Edge;
        m_dragEdge = edge;
        emit edgeDragStarted(edge.segment, edge.isIn);
        update();
        return;
    }
    m_drag = Drag::Scrub;
    emit seekRequested(int(frameAt(pt.x())));
}

void Timeline::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_stream)
        return;
    const QPointF pt = event->position();
    switch (m_drag) {
    case Drag::Scrub:
        emit seekRequested(int(frameAt(pt.x())));
        return;
    case Drag::Overview: {
        const double pos = std::clamp(pt.x(), 0.0, double(width())) / width() * frameCount();
        setView(pos - 0.5 * width() * m_framesPerPixel, m_framesPerPixel);
        return;
    }
    case Drag::Edge: {
        // An in edge sits on the left of its frame, an out edge on the right.
        const double pos = std::round(posAt(pt.x()));
        int frame = int(m_dragEdge.isIn ? pos : pos - 1);
        frame = std::clamp(frame, 0, int(frameCount()) - 1);
        emit edgeDragged(m_dragEdge.segment, m_dragEdge.isIn, frame);
        return;
    }
    case Drag::None: {
        const Edge edge = edgeAt(pt.x(), pt.y());
        if (edge.segment != m_hoverEdge.segment || edge.isIn != m_hoverEdge.isIn) {
            m_hoverEdge = edge;
            setCursor(edge.segment >= 0 ? Qt::SizeHorCursor : Qt::ArrowCursor);
            update();
        }
        return;
    }
    }
}

void Timeline::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    const Drag drag = m_drag;
    m_drag = Drag::None;
    m_dragEdge = {};
    if (drag == Drag::Edge)
        emit edgeDragFinished();
    ensureVisible(m_playhead);
    update();
}

void Timeline::leaveEvent(QEvent *)
{
    if (m_hoverEdge.segment >= 0) {
        m_hoverEdge = {};
        unsetCursor();
        update();
    }
}

void Timeline::wheelEvent(QWheelEvent *event)
{
    if (!m_stream)
        return;
    const QPoint delta = event->angleDelta();
    const bool scroll = (event->modifiers() & Qt::ShiftModifier) || std::abs(delta.x()) > std::abs(delta.y());
    const double steps = (std::abs(delta.x()) > std::abs(delta.y()) ? delta.x() : delta.y()) / 120.0;
    if (scroll) {
        setView(m_viewStart - steps * 0.1 * width() * m_framesPerPixel, m_framesPerPixel);
    } else {
        const double x = event->position().x();
        const double anchor = posAt(x);
        const double fpp = m_framesPerPixel * std::pow(1.25, -steps);
        setView(anchor - x * std::clamp(fpp, minFramesPerPixel(), maxFramesPerPixel()), fpp);
    }
    event->accept();
}

} // namespace adscutter
