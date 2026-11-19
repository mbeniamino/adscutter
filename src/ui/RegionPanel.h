#pragma once

#include "analysis/RegionDetector.h"

#include <QWidget>

class QComboBox;
class QPushButton;
class QSpinBox;
class QTreeWidget;

namespace adscutter {

// The regions to watch (method 2 for finding ads), their presets and the
// button to look for changes.
class RegionPanel : public QWidget {
    Q_OBJECT
public:
    explicit RegionPanel(QWidget *parent = nullptr);

    void setRegions(const std::vector<WatchRegion> &regions);
    void setResults(const std::vector<int> &changeCounts); // per region, -1 = not scanned
    int currentRegion() const;
    void setEnabledForRecording(bool enabled);
    int minChangeSeconds() const;
    void setMinChangeSeconds(int seconds);
    void setPresetNames(const QStringList &names, const QString &current = {});

signals:
    void drawRequested();
    void removeRequested(int index);
    void kindToggled(int index);
    void findRequested();
    void presetLoadRequested(const QString &name);
    void presetSaveRequested();
    void presetDeleteRequested(const QString &name);
    void minChangeChanged(int seconds);

private:
    QTreeWidget *m_list;
    QPushButton *m_draw, *m_remove, *m_find;
    QComboBox *m_presets;
    QPushButton *m_load, *m_save, *m_delete;
    QSpinBox *m_minChange;
    std::vector<int> m_changeCounts;
};

} // namespace adscutter
