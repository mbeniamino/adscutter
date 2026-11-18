#pragma once

#include <QWidget>

class QLabel;
class QSlider;
class QToolBar;

namespace adscutter {

// Navigation controls below the video: step buttons, a position slider
// (to be replaced by the timeline in milestone 4) and the position label.
class NavigationBar : public QWidget {
    Q_OBJECT
public:
    explicit NavigationBar(QWidget *parent = nullptr);

    void addActions(const QList<QAction *> &actions);
    void addSeparator();
    void setFrameCount(int frames); // 0 disables the slider
    void setPosition(int pos);
    void setInfo(const QString &text);

signals:
    void seekRequested(int pos);

private:
    QToolBar *m_buttons;
    QSlider *m_slider;
    QLabel *m_info;
    bool m_updating = false;
};

} // namespace adscutter
