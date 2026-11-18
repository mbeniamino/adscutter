#pragma once

#include <QWidget>

class QLabel;
class QToolBar;

namespace adscutter {

// Navigation controls below the timeline: step buttons and the position label.
class NavigationBar : public QWidget {
    Q_OBJECT
public:
    explicit NavigationBar(QWidget *parent = nullptr);

    void addActions(const QList<QAction *> &actions);
    void addSeparator();
    void setInfo(const QString &text);

private:
    QToolBar *m_buttons;
    QLabel *m_info;
};

} // namespace adscutter
