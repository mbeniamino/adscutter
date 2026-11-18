#include "analysis/AnalysisJob.h"
#include "analysis/Analyzer.h"

#include <QElapsedTimer>

namespace adscutter {

AnalysisJob::AnalysisJob(QObject *parent)
    : QObject(parent)
{
}

AnalysisJob::~AnalysisJob()
{
    cancel();
    if (m_thread)
        m_thread->wait();
}

void AnalysisJob::start(const QString &path, std::shared_ptr<const PacketIndex> index, int videoStream,
                        int audioStream, const QString &cacheDir)
{
    Q_ASSERT(!m_thread);
    m_cancel = false;
    m_thread.reset(QThread::create([this, path, index = std::move(index), videoStream, audioStream, cacheDir] {
        QElapsedTimer sinceLastReport;
        sinceLastReport.start();
        m_result = analyzeRecording(path, *index, videoStream, audioStream, [&](double fraction) {
            if (m_cancel)
                return false;
            if (sinceLastReport.elapsed() >= 100) {
                sinceLastReport.restart();
                emit progress(fraction);
            }
            return true;
        }, &m_error);
        if (m_result) {
            QString ignored;
            m_result->save(AnalysisData::cachePath(path, cacheDir), FileKey::of(path), &ignored);
        }
        emit finished();
    }));
    m_thread->start(QThread::LowPriority);
}

} // namespace adscutter
