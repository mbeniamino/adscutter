#include "ui/RegionPanel.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace adscutter {

RegionPanel::RegionPanel(QWidget *parent)
    : QWidget(parent)
    , m_list(new QTreeWidget(this))
    , m_draw(new QPushButton(tr("Draw…"), this))
    , m_remove(new QPushButton(tr("Remove"), this))
    , m_find(new QPushButton(tr("Find Changes"), this))
    , m_presets(new QComboBox(this))
    , m_load(new QPushButton(tr("Load"), this))
    , m_save(new QPushButton(tr("Save As…"), this))
    , m_delete(new QPushButton(tr("Delete"), this))
    , m_minChange(new QSpinBox(this))
{
    setObjectName(QStringLiteral("regionPanel"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_list->setObjectName(QStringLiteral("regionList"));
    m_list->setHeaderLabels({tr("Region"), tr("Shown during"), tr("Size"), tr("Changes")});
    m_list->setRootIsDecorated(false);
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_list->setToolTip(tr("Double-click \"Shown during\" to switch between programme and ads"));
    layout->addWidget(m_list, 1);

    auto *buttons = new QHBoxLayout;
    m_draw->setToolTip(tr("Drag a rectangle on the picture around a text or logo that changes between programme "
                          "and ads, on a frame where it is shown"));
    m_find->setObjectName(QStringLiteral("findChanges"));
    m_find->setToolTip(tr("Look for where the regions change in the whole recording (Ctrl+Shift+R)"));
    buttons->addWidget(m_draw);
    buttons->addWidget(m_remove);
    buttons->addStretch(1);
    buttons->addWidget(new QLabel(tr("Ignore changes shorter than"), this));
    m_minChange->setRange(1, 600);
    m_minChange->setValue(30);
    m_minChange->setSuffix(tr(" s"));
    m_minChange->setToolTip(tr("A state lasting less than this between two runs of the other one is ignored "
                               "(spots where the text is hard to see, short promos)"));
    buttons->addWidget(m_minChange);
    buttons->addWidget(m_find);
    layout->addLayout(buttons);

    auto *presets = new QHBoxLayout;
    presets->addWidget(new QLabel(tr("Presets:"), this));
    m_presets->setMinimumContentsLength(16);
    presets->addWidget(m_presets, 1);
    presets->addWidget(m_load);
    presets->addWidget(m_save);
    presets->addWidget(m_delete);
    layout->addLayout(presets);

    for (QPushButton *b : {m_draw, m_remove, m_find, m_load, m_save, m_delete})
        b->setFocusPolicy(Qt::NoFocus);
    m_minChange->setFocusPolicy(Qt::ClickFocus);

    connect(m_draw, &QPushButton::clicked, this, &RegionPanel::drawRequested);
    connect(m_remove, &QPushButton::clicked, this, [this] {
        if (currentRegion() >= 0)
            emit removeRequested(currentRegion());
    });
    connect(m_find, &QPushButton::clicked, this, &RegionPanel::findRequested);
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int column) {
        if (column == 1)
            emit kindToggled(m_list->indexOfTopLevelItem(item));
    });
    connect(m_load, &QPushButton::clicked, this, [this] {
        if (!m_presets->currentText().isEmpty())
            emit presetLoadRequested(m_presets->currentText());
    });
    connect(m_save, &QPushButton::clicked, this, &RegionPanel::presetSaveRequested);
    connect(m_delete, &QPushButton::clicked, this, [this] {
        if (!m_presets->currentText().isEmpty())
            emit presetDeleteRequested(m_presets->currentText());
    });
    connect(m_minChange, &QSpinBox::valueChanged, this, &RegionPanel::minChangeChanged);
    setEnabledForRecording(false);
    setPresetNames({});
}

void RegionPanel::setRegions(const std::vector<WatchRegion> &regions)
{
    m_list->clear();
    m_changeCounts.assign(regions.size(), -1);
    for (const WatchRegion &r : regions) {
        auto *item = new QTreeWidgetItem(m_list);
        item->setText(0, r.name);
        item->setText(1, r.kind == WatchRegion::Kind::VisibleInProgramme ? tr("programme") : tr("ads"));
        item->setText(2, QStringLiteral("%1×%2 at %3,%4").arg(r.rect.width()).arg(r.rect.height())
                             .arg(r.rect.x()).arg(r.rect.y()));
        item->setText(3, QStringLiteral("–"));
    }
    m_remove->setEnabled(!regions.empty());
    m_find->setEnabled(!regions.empty() && m_draw->isEnabled());
    m_save->setEnabled(!regions.empty());
}

void RegionPanel::setResults(const std::vector<int> &changeCounts)
{
    for (int i = 0; i < m_list->topLevelItemCount() && size_t(i) < changeCounts.size(); ++i)
        m_list->topLevelItem(i)->setText(3, changeCounts[size_t(i)] < 0 ? QStringLiteral("–")
                                                                         : QString::number(changeCounts[size_t(i)]));
}

int RegionPanel::currentRegion() const
{
    return m_list->currentItem() ? m_list->indexOfTopLevelItem(m_list->currentItem())
                                 : (m_list->topLevelItemCount() == 1 ? 0 : -1);
}

void RegionPanel::setEnabledForRecording(bool enabled)
{
    m_draw->setEnabled(enabled);
    m_find->setEnabled(enabled && m_list->topLevelItemCount() > 0);
}

int RegionPanel::minChangeSeconds() const
{
    return m_minChange->value();
}

void RegionPanel::setMinChangeSeconds(int seconds)
{
    m_minChange->setValue(seconds);
}

void RegionPanel::setPresetNames(const QStringList &names, const QString &current)
{
    m_presets->clear();
    m_presets->addItems(names);
    if (!current.isEmpty())
        m_presets->setCurrentText(current);
    m_load->setEnabled(!names.isEmpty());
    m_delete->setEnabled(!names.isEmpty());
}

} // namespace adscutter
