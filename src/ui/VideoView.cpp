#include "ui/VideoView.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

namespace adscutter {

VideoView::VideoView(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(320, 180);
    clear(tr("Open a recording with File → Open…"));
}

void VideoView::setFrame(const QImage &image, double displayAspect)
{
    m_image = image;
    m_aspect = displayAspect > 0.0 ? displayAspect : double(image.width()) / image.height();
    m_message.clear();
    update();
}

void VideoView::clear(const QString &message)
{
    m_image = QImage();
    m_message = message;
    update();
}

void VideoView::setOverlays(const QList<Overlay> &overlays)
{
    m_overlays = overlays;
    update();
}

void VideoView::setMarker(const QString &text, const QColor &color)
{
    m_markerText = text;
    m_markerColor = color;
    update();
}

void VideoView::clearMarker()
{
    if (m_markerText.isEmpty())
        return;
    m_markerText.clear();
    update();
}

void VideoView::setDrawing(bool drawing)
{
    m_drawing = drawing;
    m_dragging = false;
    setCursor(drawing ? Qt::CrossCursor : Qt::ArrowCursor);
    if (drawing)
        setFocus(); // for Esc
    update();
}

QRectF VideoView::targetRect() const
{
    // Fit a rectangle with the display aspect ratio into the widget.
    const double widgetAspect = double(width()) / height();
    if (widgetAspect > m_aspect) {
        const double w = height() * m_aspect;
        return QRectF((width() - w) / 2.0, 0, w, height());
    }
    const double h = width() / m_aspect;
    return QRectF(0, (height() - h) / 2.0, width(), h);
}

QPointF VideoView::toPicture(const QPointF &widget) const
{
    const QRectF t = targetRect();
    if (m_image.isNull() || t.isEmpty())
        return {};
    return QPointF((widget.x() - t.left()) * m_image.width() / t.width(),
                   (widget.y() - t.top()) * m_image.height() / t.height());
}

QRectF VideoView::toWidget(const QRect &picture) const
{
    const QRectF t = targetRect();
    if (m_image.isNull())
        return {};
    const double sx = t.width() / m_image.width(), sy = t.height() / m_image.height();
    return QRectF(t.left() + picture.x() * sx, t.top() + picture.y() * sy, picture.width() * sx,
                  picture.height() * sy);
}

void VideoView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);

    if (m_image.isNull()) {
        p.setPen(palette().color(QPalette::PlaceholderText));
        p.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap, m_message);
        return;
    }

    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(targetRect(), m_image);

    // Regions.
    QFont font = p.font();
    font.setPointSizeF(font.pointSizeF() * 0.9);
    p.setFont(font);
    for (const Overlay &o : std::as_const(m_overlays)) {
        const QRectF r = toWidget(o.rect);
        p.setPen(QPen(o.color, 2));
        p.setBrush(Qt::NoBrush);
        p.drawRect(r);
        if (!o.label.isEmpty()) {
            const QRectF text = p.fontMetrics().boundingRect(o.label).adjusted(-3, -1, 3, 1);
            const QRectF box(r.left(), r.top() - text.height() - 1, text.width(), text.height());
            p.fillRect(box, o.color);
            p.setPen(Qt::black);
            p.drawText(box, Qt::AlignCenter, o.label);
        }
    }

    // Marker: a thick frame round the picture, the caption centred on top.
    if (!m_markerText.isEmpty()) {
        const QRectF t = targetRect();
        const double w = std::max(4.0, t.height() / 60.0);
        p.setPen(QPen(m_markerColor, w, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
        p.setBrush(Qt::NoBrush);
        p.drawRect(t.adjusted(w / 2, w / 2, -w / 2, -w / 2));
        QFont big = p.font();
        big.setPointSizeF(big.pointSizeF() / 0.9 * 1.3);
        big.setBold(true);
        p.setFont(big);
        const QRectF text = p.fontMetrics().boundingRect(m_markerText).adjusted(-10, -4, 10, 4);
        const QRectF box(t.center().x() - text.width() / 2, t.top() + w, text.width(), text.height());
        p.fillRect(box, m_markerColor);
        p.setPen(Qt::black);
        p.drawText(box, Qt::AlignCenter, m_markerText);
        p.setFont(font);
    }

    // Rectangle being drawn.
    if (m_drawing) {
        p.setPen(QPen(Qt::yellow, 1, Qt::DashLine));
        if (m_dragging)
            p.drawRect(QRectF(m_dragStart, m_dragEnd).normalized());
        const QString hint = tr("Drag a rectangle around the text or logo · Esc to cancel");
        const QRectF box = p.fontMetrics().boundingRect(hint).adjusted(-6, -3, 6, 3).translated(10 - 0, 10 + 12);
        p.fillRect(box, QColor(0, 0, 0, 160));
        p.setPen(Qt::yellow);
        p.drawText(box, Qt::AlignCenter, hint);
    }
}

void VideoView::mousePressEvent(QMouseEvent *event)
{
    if (!m_drawing || event->button() != Qt::LeftButton || m_image.isNull())
        return QWidget::mousePressEvent(event);
    m_dragging = true;
    m_dragStart = m_dragEnd = event->position();
    update();
}

void VideoView::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging)
        return QWidget::mouseMoveEvent(event);
    m_dragEnd = event->position();
    update();
}

void VideoView::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_dragging || event->button() != Qt::LeftButton)
        return QWidget::mouseReleaseEvent(event);
    m_dragging = false;
    const QRectF widget = QRectF(m_dragStart, event->position()).normalized();
    const QPointF a = toPicture(widget.topLeft()), b = toPicture(widget.bottomRight());
    const QRect picture = QRect(QPoint(int(a.x()), int(a.y())), QPoint(int(b.x()), int(b.y())))
                              .intersected(QRect(0, 0, m_image.width(), m_image.height()));
    setDrawing(false);
    if (picture.width() >= 8 && picture.height() >= 8)
        emit rectangleDrawn(picture);
    else
        emit drawingCanceled();
}

void VideoView::keyPressEvent(QKeyEvent *event)
{
    if (m_drawing && event->key() == Qt::Key_Escape) {
        setDrawing(false);
        emit drawingCanceled();
        return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace adscutter
