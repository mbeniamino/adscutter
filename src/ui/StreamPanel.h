#pragma once

#include "media/MediaFile.h"

#include <QWidget>
#include <vector>

class QTreeWidget;

namespace adscutter {

// Lists the streams of the open file with a checkbox to select which ones
// will be exported.
class StreamPanel : public QWidget {
    Q_OBJECT
public:
    explicit StreamPanel(QWidget *parent = nullptr);

    void setStreams(const std::vector<StreamInfo> &streams, int mainVideo);
    void clear();
    std::vector<int> selectedStreams() const;
    void setSelectedStreams(const std::vector<int> &streams); // only exportable ones

signals:
    void selectionChanged();

private:
    QTreeWidget *m_tree;
};

} // namespace adscutter
