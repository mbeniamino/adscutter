#include "media/IndexJob.h"
#include "media/IndexBuilder.h"

#include <QElapsedTimer>

namespace adscutter {

namespace {

// The cache is keyed by file path, size and mtime; also make sure the stream
// layout matches what the demuxer sees now.
bool sameStreams(const PacketIndex &index, const AVFormatContext *fmt)
{
    if (index.streams().size() != fmt->nb_streams)
        return false;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const PacketIndex::Stream &s = index.streams()[i];
        const AVStream *st = fmt->streams[i];
        if (s.index != int(i) || s.codecId != st->codecpar->codec_id
            || av_cmp_q(s.timeBase, st->time_base) != 0)
            return false;
    }
    return true;
}

} // namespace

IndexJob::IndexJob(QObject *parent)
    : QObject(parent)
{
}

IndexJob::~IndexJob()
{
    cancel();
    if (m_thread)
        m_thread->wait();
}

void IndexJob::start(FormatContextPtr fmt, const QString &mediaPath, const QString &cacheDir, bool useCache)
{
    Q_ASSERT(!m_thread);
    m_cancel = false;
    // std::function needs a copyable callable: hand the context over raw.
    AVFormatContext *raw = fmt.release();
    m_thread.reset(QThread::create([this, raw, mediaPath, cacheDir, useCache] {
        run(FormatContextPtr(raw), mediaPath, cacheDir, useCache);
    }));
    m_thread->start(QThread::LowPriority);
}

void IndexJob::cancel()
{
    m_cancel = true;
}

bool IndexJob::isRunning() const
{
    return m_thread && m_thread->isRunning();
}

std::unique_ptr<PacketIndex> IndexJob::takeResult()
{
    return std::move(m_result);
}

void IndexJob::run(FormatContextPtr fmt, QString mediaPath, QString cacheDir, bool useCache)
{
    const QString cacheFile = PacketIndex::cachePath(mediaPath, cacheDir);
    const FileKey key = FileKey::of(mediaPath);

    if (useCache) {
        QString ignored;
        auto cached = PacketIndex::load(cacheFile, key, &ignored);
        if (cached && sameStreams(*cached, fmt.get())) {
            m_result = std::move(cached);
            m_fromCache = true;
            emit finished();
            return;
        }
    }

    QElapsedTimer sinceLastReport;
    sinceLastReport.start();
    QString error;
    auto index = buildIndex(fmt.get(), [&](double fraction) {
        if (m_cancel)
            return false;
        if (sinceLastReport.elapsed() >= 100) {
            sinceLastReport.restart();
            emit progress(fraction);
        }
        return true;
    }, &error);
    fmt.reset(); // close the file in this thread

    if (!index) {
        if (!m_cancel)
            emit failed(error.isEmpty() ? QStringLiteral("Indexing failed") : error);
        return;
    }

    QString saveError;
    if (!index->save(cacheFile, key, &saveError))
        qWarning("Cannot write index cache %s: %s", qPrintable(cacheFile), qPrintable(saveError));

    m_result = std::move(index);
    m_fromCache = false;
    emit finished();
}

} // namespace adscutter
