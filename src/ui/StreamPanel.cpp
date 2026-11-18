#include "ui/StreamPanel.h"

#include <QHeaderView>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace adscutter {

namespace {
enum Column { ColIndex, ColType, ColLanguage, ColCodec, ColDetails, ColCount };
}

StreamPanel::StreamPanel(QWidget *parent)
    : QWidget(parent)
    , m_tree(new QTreeWidget(this))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_tree);

    m_tree->setColumnCount(ColCount);
    m_tree->setHeaderLabels({tr("#"), tr("Type"), tr("Lang"), tr("Codec"), tr("Details")});
    m_tree->setRootIsDecorated(false);
    m_tree->setUniformRowHeights(true);
    m_tree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_tree->header()->setStretchLastSection(true);

    connect(m_tree, &QTreeWidget::itemChanged, this,
            [this](QTreeWidgetItem *, int column) {
                if (column == ColIndex)
                    emit selectionChanged();
            });
}

void StreamPanel::setStreams(const std::vector<StreamInfo> &streams, int mainVideo)
{
    QSignalBlocker blocker(m_tree);
    m_tree->clear();

    for (const StreamInfo &s : streams) {
        auto *item = new QTreeWidgetItem(m_tree);
        item->setText(ColIndex, QString::number(s.index));
        item->setData(ColIndex, Qt::UserRole, s.index);
        item->setText(ColType, s.typeName());
        item->setText(ColLanguage, s.language);
        item->setText(ColCodec, s.codecName);
        QString details = s.details();
        if (!s.title.isEmpty())
            details = s.title + QStringLiteral(" — ") + details;
        item->setText(ColDetails, details);

        // Default selection: the main video stream plus every audio and
        // subtitle stream that can be stored in the output.
        bool checked = false;
        if (s.type == AVMEDIA_TYPE_VIDEO)
            checked = s.index == mainVideo;
        else if (s.type == AVMEDIA_TYPE_AUDIO || s.type == AVMEDIA_TYPE_SUBTITLE)
            checked = true;
        checked = checked && s.mkvCompatible;

        Qt::ItemFlags flags = Qt::ItemIsSelectable | Qt::ItemNeverHasChildren;
        if (s.mkvCompatible)
            flags |= Qt::ItemIsEnabled | Qt::ItemIsUserCheckable;
        item->setFlags(flags);
        item->setCheckState(ColIndex, checked ? Qt::Checked : Qt::Unchecked);

        if (!s.exportNote.isEmpty()) {
            for (int c = 0; c < ColCount; ++c)
                item->setToolTip(c, s.exportNote);
        }
    }
    emit selectionChanged();
}

void StreamPanel::clear()
{
    m_tree->clear();
    emit selectionChanged();
}

std::vector<int> StreamPanel::selectedStreams() const
{
    std::vector<int> result;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        const QTreeWidgetItem *item = m_tree->topLevelItem(i);
        if (item->checkState(ColIndex) == Qt::Checked)
            result.push_back(item->data(ColIndex, Qt::UserRole).toInt());
    }
    return result;
}

void StreamPanel::setSelectedStreams(const std::vector<int> &streams)
{
    {
        QSignalBlocker blocker(m_tree);
        for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *item = m_tree->topLevelItem(i);
            if (!(item->flags() & Qt::ItemIsUserCheckable))
                continue;
            const int index = item->data(ColIndex, Qt::UserRole).toInt();
            const bool on = std::find(streams.begin(), streams.end(), index) != streams.end();
            item->setCheckState(ColIndex, on ? Qt::Checked : Qt::Unchecked);
        }
    }
    emit selectionChanged();
}

} // namespace adscutter
