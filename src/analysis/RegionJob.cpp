#include "analysis/RegionJob.h"

#include <QElapsedTimer>
#include <QJsonObject>

namespace adscutter {

QJsonArray regionsToJson(const std::vector<WatchRegion> &regions)
{
    QJsonArray array;
    for (const WatchRegion &r : regions) {
        QByteArray edges(reinterpret_cast<const char *>(r.templateEdges.data()), qsizetype(r.templateEdges.size()));
        array.append(QJsonObject{
            {QStringLiteral("name"), r.name},
            {QStringLiteral("kind"), r.kind == WatchRegion::Kind::VisibleInProgramme ? QStringLiteral("programme")
                                                                                     : QStringLiteral("ads")},
            {QStringLiteral("rect"), QJsonArray{r.rect.x(), r.rect.y(), r.rect.width(), r.rect.height()}},
            {QStringLiteral("picture"), QJsonArray{r.pictureSize.width(), r.pictureSize.height()}},
            {QStringLiteral("template"), QJsonArray{r.templateSize.width(), r.templateSize.height()}},
            {QStringLiteral("edges"), QString::fromLatin1(qCompress(edges).toBase64())},
        });
    }
    return array;
}

std::vector<WatchRegion> regionsFromJson(const QJsonArray &array)
{
    std::vector<WatchRegion> regions;
    for (const QJsonValue &v : array) {
        const QJsonObject o = v.toObject();
        WatchRegion r;
        r.name = o.value(QStringLiteral("name")).toString();
        r.kind = o.value(QStringLiteral("kind")).toString() == QStringLiteral("programme")
                     ? WatchRegion::Kind::VisibleInProgramme
                     : WatchRegion::Kind::VisibleInAds;
        const QJsonArray rect = o.value(QStringLiteral("rect")).toArray();
        const QJsonArray picture = o.value(QStringLiteral("picture")).toArray();
        const QJsonArray size = o.value(QStringLiteral("template")).toArray();
        r.rect = QRect(rect.at(0).toInt(), rect.at(1).toInt(), rect.at(2).toInt(), rect.at(3).toInt());
        r.pictureSize = QSize(picture.at(0).toInt(), picture.at(1).toInt());
        r.templateSize = QSize(size.at(0).toInt(), size.at(1).toInt());
        const QByteArray edges = qUncompress(QByteArray::fromBase64(o.value(QStringLiteral("edges")).toString().toLatin1()));
        r.templateEdges.assign(edges.begin(), edges.end());
        if (r.isValid() && qsizetype(r.templateSize.width()) * r.templateSize.height() == edges.size())
            regions.push_back(std::move(r));
    }
    return regions;
}

RegionJob::RegionJob(QObject *parent)
    : QObject(parent)
{
}

RegionJob::~RegionJob()
{
    cancel();
    if (m_thread)
        m_thread->wait();
}

void RegionJob::start(const QString &path, std::shared_ptr<const PacketIndex> index, int videoStream,
                      std::vector<WatchRegion> regions, const RegionScanSettings &settings)
{
    Q_ASSERT(!m_thread);
    m_cancel = false;
    m_thread.reset(QThread::create([this, path, index = std::move(index), videoStream, regions = std::move(regions),
                                    settings] {
        QElapsedTimer sinceLastReport;
        sinceLastReport.start();
        m_results = scanRegions(path, index, videoStream, regions, [&](double fraction) {
            if (m_cancel)
                return false;
            if (sinceLastReport.elapsed() >= 100) {
                sinceLastReport.restart();
                emit progress(fraction);
            }
            return true;
        }, &m_error, settings);
        emit finished();
    }));
    m_thread->start(QThread::LowPriority);
}

} // namespace adscutter
