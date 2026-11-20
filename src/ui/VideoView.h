#pragma once

#include <QImage>
#include <QWidget>

namespace adscutter {

// Displays a decoded frame, letterboxed and aspect-ratio corrected.
//
// It can also show rectangles over the picture (in picture pixels), a
// marker (a coloured frame round the picture with a caption, flashed on
// the frame a previewed cue or cut is on) and, in drawing mode, let the
// user drag a new one.
class VideoView : public QWidget {
    Q_OBJECT
public:
    struct Overlay {
        QRect rect; // picture pixels
        QColor color;
        QString label;
    };

    explicit VideoView(QWidget *parent = nullptr);

    void setFrame(const QImage &image, double displayAspect);
    void clear(const QString &message = {});
    void setOverlays(const QList<Overlay> &overlays);
    void setMarker(const QString &text, const QColor &color);
    void clearMarker();
    const QString &markerText() const { return m_markerText; }
    void setDrawing(bool drawing);
    bool isDrawing() const { return m_drawing; }

    QSize sizeHint() const override { return {768, 432}; }

signals:
    void rectangleDrawn(const QRect &pictureRect);
    void drawingCanceled();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    QRectF targetRect() const; // where the picture is drawn
    QPointF toPicture(const QPointF &widget) const;
    QRectF toWidget(const QRect &picture) const;

    QImage m_image;
    double m_aspect = 16.0 / 9.0;
    QString m_message;
    QList<Overlay> m_overlays;
    QString m_markerText; // empty: no marker
    QColor m_markerColor;
    bool m_drawing = false;
    bool m_dragging = false;
    QPointF m_dragStart, m_dragEnd; // widget coordinates
};

} // namespace adscutter
