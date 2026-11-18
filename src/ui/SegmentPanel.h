#pragma once

#include <QWidget>
#include <cstdint>
#include <functional>

class QLabel;
class QTreeWidget;

namespace adscutter {

class CutList;

// Lists the kept segments with their times and the total kept duration.
class SegmentPanel : public QWidget {
    Q_OBJECT
public:
    explicit SegmentPanel(QWidget *parent = nullptr);

    // timeOf(frame) gives the unified time in microseconds; totalUs is the
    // duration of the whole recording.
    void refresh(const CutList *cutList, const std::function<int64_t(uint32_t)> &timeOf, int64_t frameUs,
                 int64_t totalUs);
    void setCurrentSegment(int index);

signals:
    void segmentActivated(int index);

private:
    QTreeWidget *m_tree;
    QLabel *m_summary;
};

} // namespace adscutter
