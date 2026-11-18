#include "ui/NavigationBar.h"

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QToolBar>
#include <QVBoxLayout>

namespace adscutter {

NavigationBar::NavigationBar(QWidget *parent)
    : QWidget(parent)
    , m_buttons(new QToolBar(this))
    , m_slider(new QSlider(Qt::Horizontal, this))
    , m_info(new QLabel(this))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 4, 6, 4);
    layout->setSpacing(2);

    // Keyboard navigation is handled by window-wide shortcuts; the slider
    // must not steal the arrow keys.
    m_slider->setFocusPolicy(Qt::NoFocus);
    m_slider->setEnabled(false);
    layout->addWidget(m_slider);

    auto *row = new QHBoxLayout;
    m_buttons->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_buttons->setIconSize(QSize(16, 16));
    row->addWidget(m_buttons);
    row->addStretch(1);
    m_info->setObjectName(QStringLiteral("positionInfo"));
    m_info->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    row->addWidget(m_info);
    layout->addLayout(row);

    connect(m_slider, &QSlider::valueChanged, this, [this](int value) {
        if (!m_updating)
            emit seekRequested(value);
    });
}

void NavigationBar::addActions(const QList<QAction *> &actions)
{
    m_buttons->addActions(actions);
}

void NavigationBar::addSeparator()
{
    m_buttons->addSeparator();
}

void NavigationBar::setFrameCount(int frames)
{
    m_updating = true;
    m_slider->setRange(0, std::max(0, frames - 1));
    m_slider->setPageStep(std::max(1, frames / 50));
    m_slider->setEnabled(frames > 0);
    m_updating = false;
}

void NavigationBar::setPosition(int pos)
{
    if (m_slider->isSliderDown())
        return; // don't fight the user while dragging
    m_updating = true;
    m_slider->setValue(pos);
    m_updating = false;
}

void NavigationBar::setInfo(const QString &text)
{
    m_info->setText(text);
}

} // namespace adscutter
