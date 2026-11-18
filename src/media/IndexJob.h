#pragma once

#include "media/PacketIndex.h"

#include <QObject>
#include <QThread>
#include <atomic>
#include <memory>

namespace adscutter {

// Loads a cached index or builds a new one in a background thread.
//
// Signals are emitted from the worker thread; connect with the default
// (auto) connection type so slots run in the receiver's thread.
class IndexJob : public QObject {
    Q_OBJECT
public:
    explicit IndexJob(QObject *parent = nullptr);
    ~IndexJob() override; // cancels and waits for the worker

    // Takes ownership of a freshly opened format context for mediaPath.
    // cacheDir empty = default cache location; useCache false = always rebuild.
    void start(FormatContextPtr fmt, const QString &mediaPath, const QString &cacheDir = {},
               bool useCache = true);
    void cancel();
    bool isRunning() const;

    // Valid after finished(); transfers ownership.
    std::unique_ptr<PacketIndex> takeResult();
    bool loadedFromCache() const { return m_fromCache; }

signals:
    void progress(double fraction);
    void finished();
    void failed(const QString &error);

private:
    void run(FormatContextPtr fmt, QString mediaPath, QString cacheDir, bool useCache);

    std::unique_ptr<QThread> m_thread;
    std::atomic<bool> m_cancel{false};
    std::unique_ptr<PacketIndex> m_result;
    bool m_fromCache = false;
};

} // namespace adscutter
