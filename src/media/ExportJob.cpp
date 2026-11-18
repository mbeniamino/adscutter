#include "media/ExportJob.h"

#include <QElapsedTimer>

namespace adscutter {

ExportJob::ExportJob(QObject *parent)
    : QObject(parent)
{
}

ExportJob::~ExportJob()
{
    cancel();
    if (m_thread)
        m_thread->wait();
}

void ExportJob::start(const QString &inputPath, std::shared_ptr<const PacketIndex> index,
                      const ExportSettings &settings)
{
    Q_ASSERT(!m_thread);
    m_cancel = false;
    m_thread.reset(QThread::create([this, inputPath, index = std::move(index), settings] {
        QElapsedTimer sinceLastReport;
        sinceLastReport.start();
        m_ok = exportMatroska(inputPath, *index, settings, [&](double fraction) {
            if (m_cancel)
                return false;
            if (sinceLastReport.elapsed() >= 100) {
                sinceLastReport.restart();
                emit progress(fraction);
            }
            return true;
        }, &m_report, &m_error);
        emit finished();
    }));
    m_thread->start();
}

} // namespace adscutter
