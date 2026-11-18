#include "ui/NavigationBar.h"

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolBar>

namespace adscutter {

NavigationBar::NavigationBar(QWidget *parent)
    : QWidget(parent)
    , m_buttons(new QToolBar(this))
    , m_info(new QLabel(this))
{
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(6, 0, 6, 4);
    m_buttons->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_buttons->setIconSize(QSize(16, 16));
    m_info->setObjectName(QStringLiteral("positionInfo"));
    m_info->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // The buttons have priority: the label takes what is left (clipped if
    // needed; the full text is in the tooltip).
    m_info->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_info->setMinimumWidth(0);
    m_info->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_buttons->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    row->addWidget(m_buttons);
    row->addWidget(m_info, 1);
}

void NavigationBar::addActions(const QList<QAction *> &actions)
{
    m_buttons->addActions(actions);
}

void NavigationBar::addSeparator()
{
    m_buttons->addSeparator();
}

void NavigationBar::setInfo(const QString &text)
{
    m_info->setText(text);
    m_info->setToolTip(text);
}

} // namespace adscutter
