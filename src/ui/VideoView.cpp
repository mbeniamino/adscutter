#include "ui/VideoView.h"

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

void VideoView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);

    if (m_image.isNull()) {
        p.setPen(palette().color(QPalette::PlaceholderText));
        p.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap, m_message);
        return;
    }

    // Fit a rectangle with the display aspect ratio into the widget.
    const double widgetAspect = double(width()) / height();
    QRectF target;
    if (widgetAspect > m_aspect) {
        const double w = height() * m_aspect;
        target = QRectF((width() - w) / 2.0, 0, w, height());
    } else {
        const double h = width() / m_aspect;
        target = QRectF(0, (height() - h) / 2.0, width(), h);
    }

    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(target, m_image);
}

} // namespace adscutter
