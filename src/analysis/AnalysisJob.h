#pragma once

#include "analysis/AnalysisData.h"

#include <QObject>
#include <QThread>
#include <atomic>
#include <memory>

namespace adscutter {

// Runs analyzeRecording() in a background thread and caches the result.
class AnalysisJob : public QObject {
    Q_OBJECT
public:
    explicit AnalysisJob(QObject *parent = nullptr);
    ~AnalysisJob() override; // cancels and waits

    void start(const QString &path, std::shared_ptr<const PacketIndex> index, int videoStream, int audioStream,
               const QString &cacheDir = {});
    void cancel() { m_cancel = true; }

    // Valid after finished(): the result (nullptr if failed or canceled).
    std::unique_ptr<AnalysisData> takeResult() { return std::move(m_result); }
    const QString &error() const { return m_error; }
    bool canceled() const { return m_cancel && !m_result; }

signals:
    void progress(double fraction); // from the worker thread
    void finished();

private:
    std::unique_ptr<QThread> m_thread;
    std::atomic<bool> m_cancel{false};
    std::unique_ptr<AnalysisData> m_result;
    QString m_error;
};

} // namespace adscutter
