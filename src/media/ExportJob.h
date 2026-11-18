#pragma once

#include "media/Exporter.h"

#include <QObject>
#include <QThread>
#include <atomic>
#include <memory>

namespace adscutter {

// Runs exportMatroska() in a background thread.
class ExportJob : public QObject {
    Q_OBJECT
public:
    explicit ExportJob(QObject *parent = nullptr);
    ~ExportJob() override; // cancels and waits

    void start(const QString &inputPath, std::shared_ptr<const PacketIndex> index, const ExportSettings &settings);
    void cancel() { m_cancel = true; }

    // Valid after finished().
    bool succeeded() const { return m_ok; }
    bool canceled() const { return m_cancel && !m_ok; }
    const QString &error() const { return m_error; }
    const ExportReport &report() const { return m_report; }

signals:
    void progress(double fraction); // from the worker thread
    void finished();

private:
    std::unique_ptr<QThread> m_thread;
    std::atomic<bool> m_cancel{false};
    bool m_ok = false;
    QString m_error;
    ExportReport m_report;
};

} // namespace adscutter
