#pragma once

#include "media/PacketIndex.h"

#include <QWidget>
#include <memory>

namespace adscutter {

class CutList;

// Zoomable timeline of the main video stream: time ruler, kept segments,
// pending marks, cut points (when zoomed in), playhead and an overview strip.
//
// Mouse: click/drag to scrub, drag a segment edge to move it (starts snap
// to cut points), wheel to zoom around the pointer, Shift+wheel to scroll.
class Timeline : public QWidget {
    Q_OBJECT
public:
    explicit Timeline(QWidget *parent = nullptr);

    void setSource(std::shared_ptr<const PacketIndex> index, int streamIndex);
    void clearSource();
    void setCutList(const CutList *cutList) { m_cutList = cutList; update(); }
    void setPlayhead(uint32_t pos);

    void zoomIn();
    void zoomOut();
    void zoomFit();
    double framesPerPixel() const { return m_framesPerPixel; }

    QSize sizeHint() const override { return {800, 76}; }
    QSize minimumSizeHint() const override { return {200, 76}; }

signals:
    void seekRequested(int pos);
    void edgeDragStarted(int segment, bool isIn);
    void edgeDragged(int segment, bool isIn, int pos);
    void edgeDragFinished();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    struct Edge {
        int segment = -1;
        bool isIn = false;
    };

    uint32_t frameCount() const;
    double xOf(double pos) const { return (pos - m_viewStart) / m_framesPerPixel; }
    double posAt(double x) const { return m_viewStart + x * m_framesPerPixel; }
    uint32_t frameAt(double x) const;
    double maxFramesPerPixel() const;
    double minFramesPerPixel() const { return 1.0 / 16.0; } // 16 px per frame
    void setView(double start, double framesPerPixel);
    void ensureVisible(uint32_t pos);
    Edge edgeAt(double x, double y) const;
    QRectF trackRect() const;
    QRectF overviewRect() const;
    QRectF rulerRect() const;

    void paintRuler(QPainter &p);
    void paintTrack(QPainter &p);
    void paintOverview(QPainter &p);

    std::shared_ptr<const PacketIndex> m_index;
    const PacketIndex::Stream *m_stream = nullptr;
    const CutList *m_cutList = nullptr;
    uint32_t m_playhead = 0;
    double m_viewStart = 0.0;
    double m_framesPerPixel = 1.0;
    bool m_fitted = true; // keep fitting the whole file on resize

    enum class Drag { None, Scrub, Edge, Overview };
    Drag m_drag = Drag::None;
    Edge m_dragEdge;
    Edge m_hoverEdge;
};

} // namespace adscutter
