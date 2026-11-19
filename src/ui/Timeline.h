#pragma once

#include "analysis/AnalysisData.h"
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
    // Analysis lanes (darkness, loudness) and the breaks found; nullptr hides them.
    void setAnalysis(const AnalysisData *analysis, std::vector<BreakCandidate> candidates,
                     std::vector<BreakRegion> breaks);
    // One lane per watched region: where it is shown, and its changes.
    struct RegionLane {
        QString name;
        QColor color;
        std::vector<std::pair<uint32_t, uint32_t>> shown;
        std::vector<uint32_t> changes;
    };
    void setRegionLanes(std::vector<RegionLane> lanes);
    void setPlayhead(uint32_t pos);

    void zoomIn();
    void zoomOut();
    void zoomFit();
    double framesPerPixel() const { return m_framesPerPixel; }

    QSize sizeHint() const override { return {800, totalHeight()}; }
    QSize minimumSizeHint() const override { return {200, totalHeight()}; }

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
    int laneCount() const;
    QRectF laneRect(int lane) const;
    int totalHeight() const;
    void computeColumns(); // per-pixel aggregates of the lanes for the current view
    void paintLanes(QPainter &p);

    void paintRuler(QPainter &p);
    void paintTrack(QPainter &p);
    void paintOverview(QPainter &p);

    std::shared_ptr<const PacketIndex> m_index;
    const PacketIndex::Stream *m_stream = nullptr;
    const CutList *m_cutList = nullptr;
    const AnalysisData *m_analysis = nullptr;
    std::vector<BreakCandidate> m_candidates;
    std::vector<BreakRegion> m_breaks;
    std::vector<uint32_t> m_subtitleFrames; // frames where a subtitle starts
    std::vector<RegionLane> m_regionLanes;
    // Cached per-column lane values, valid for m_columnsKey.
    std::vector<uint8_t> m_darkColumns, m_loudColumns;
    std::tuple<double, double, int> m_columnsKey{-1, -1, -1};
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
