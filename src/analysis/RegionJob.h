#pragma once

#include "analysis/RegionDetector.h"

#include <QJsonArray>
#include <QObject>
#include <QThread>
#include <atomic>
#include <memory>

namespace adscutter {

// Regions to/from JSON (for presets and settings).
QJsonArray regionsToJson(const std::vector<WatchRegion> &regions);
std::vector<WatchRegion> regionsFromJson(const QJsonArray &array);

// Runs scanRegions() in a background thread.
class RegionJob : public QObject {
    Q_OBJECT
public:
    explicit RegionJob(QObject *parent = nullptr);
    ~RegionJob() override; // cancels and waits

    void start(const QString &path, std::shared_ptr<const PacketIndex> index, int videoStream,
               std::vector<WatchRegion> regions, const RegionScanSettings &settings);
    void cancel() { m_cancel = true; }

    // Valid after finished().
    const std::vector<RegionResult> &results() const { return m_results; }
    const QString &error() const { return m_error; }
    bool succeeded() const { return !m_results.empty(); }
    bool canceled() const { return m_cancel && m_results.empty(); }

signals:
    void progress(double fraction); // from the worker thread
    void finished();

private:
    std::unique_ptr<QThread> m_thread;
    std::atomic<bool> m_cancel{false};
    std::vector<RegionResult> m_results;
    QString m_error;
};

} // namespace adscutter
