#include "ui/SegmentPanel.h"
#include "model/CutList.h"

#include <QHeaderView>
#include <QLabel>
#include <QTime>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace adscutter {

namespace {

QString formatTime(int64_t us)
{
    return QTime(0, 0).addMSecs(std::max<int64_t>(0, us) / 1000).toString(QStringLiteral("H:mm:ss.zzz"));
}

} // namespace

SegmentPanel::SegmentPanel(QWidget *parent)
    : QWidget(parent)
    , m_tree(new QTreeWidget(this))
    , m_summary(new QLabel(this))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_tree, 1);
    layout->addWidget(m_summary);

    m_tree->setObjectName(QStringLiteral("segmentList"));
    m_tree->setColumnCount(5);
    m_tree->setHeaderLabels({tr("#"), tr("Start"), tr("End"), tr("Duration"), tr("Frames")});
    m_tree->setRootIsDecorated(false);
    m_tree->setUniformRowHeights(true);
    m_tree->setFocusPolicy(Qt::NoFocus);
    m_tree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_summary->setObjectName(QStringLiteral("segmentSummary"));
    m_summary->setContentsMargins(4, 2, 4, 2);

    connect(m_tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
        emit segmentActivated(m_tree->indexOfTopLevelItem(item));
    });
    connect(m_tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) {
        emit segmentActivated(m_tree->indexOfTopLevelItem(item));
    });
}

void SegmentPanel::refresh(const CutList *cutList, const std::function<int64_t(uint32_t)> &timeOf,
                           int64_t frameUs, int64_t totalUs)
{
    QSignalBlocker blocker(m_tree);
    m_tree->clear();
    if (!cutList || cutList->frameCount() == 0) {
        m_summary->clear();
        return;
    }

    int64_t keptUs = 0;
    const auto &segs = cutList->segments();
    for (size_t i = 0; i < segs.size(); ++i) {
        const int64_t start = timeOf(segs[i].in);
        const int64_t end = timeOf(segs[i].out) + frameUs; // out frame is included
        keptUs += end - start;
        auto *item = new QTreeWidgetItem(m_tree);
        item->setText(0, QString::number(i + 1));
        item->setText(1, formatTime(start));
        item->setText(2, formatTime(end));
        item->setText(3, formatTime(end - start));
        item->setText(4, QStringLiteral("%1–%2").arg(segs[i].in + 1).arg(segs[i].out + 1));
        for (int c = 0; c < 5; ++c)
            item->setTextAlignment(c, Qt::AlignRight | Qt::AlignVCenter);
    }

    if (segs.empty()) {
        m_summary->setText(tr("No segments yet: mark the start (I) and end (O) of each part to keep."));
    } else {
        m_summary->setText(tr("Keeping %1 of %2 in %n segment(s)", nullptr, int(segs.size()))
                               .arg(formatTime(keptUs), formatTime(totalUs)));
    }
}

void SegmentPanel::setCurrentSegment(int index)
{
    QSignalBlocker blocker(m_tree);
    if (index >= 0 && index < m_tree->topLevelItemCount())
        m_tree->setCurrentItem(m_tree->topLevelItem(index));
    else
        m_tree->clearSelection();
}

} // namespace adscutter
