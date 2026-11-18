#include "ui/Timeline.h"
#include "model/CutList.h"

#include <QMouseEvent>
#include <QPainter>
#include <QTime>
#include <QWheelEvent>
#include <cmath>

namespace adscutter {

namespace {

constexpr int kRulerHeight = 20;
constexpr int kTrackTop = 22;
constexpr int kTrackHeight = 34;
constexpr int kOverviewTop = 62;
constexpr int kOverviewHeight = 10;
constexpr double kEdgeGrabPx = 5.0;

const QColor kKept(76, 160, 92);
const QColor kRemoved(160, 64, 64, 70);
const QColor kInMark(52, 120, 220);
const QColor kOutMark(230, 140, 30);
const QColor kPlayhead(230, 40, 40);

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
                  "Wheel: zoom · Shift+wheel: scroll · bottom strip: whole recording"));
}

void Timeline::setSource(std::shared_ptr<const PacketIndex> index, int streamIndex)
{
    m_index = std::move(index);
    m_stream = m_index ? m_index->stream(streamIndex) : nullptr;
    if (m_stream && m_stream->displayOrder.empty())
        m_stream = nullptr;
    m_playhead = 0;
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

QRectF Timeline::overviewRect() const
{
    return QRectF(0, kOverviewTop, width(), kOverviewHeight);
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
    if (pt.y() >= kOverviewTop - 2) {
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
