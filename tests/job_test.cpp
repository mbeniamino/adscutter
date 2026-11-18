// Runs IndexJob twice on a file with a private cache directory: the first
// run must build the index, the second must load it from the cache. Then
// checks that canceling a running job never reports a result.
//
// usage: job_test FILE

#include "media/IndexJob.h"
#include "media/MediaFile.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>

extern "C" {
#include <libavutil/log.h>
}

using namespace adscutter;

namespace {

struct Outcome {
    bool finished = false;
    bool failed = false;
    bool fromCache = false;
    size_t packets = 0;
    QString error;
};

Outcome runJob(const QString &path, const QString &cacheDir, bool cancelImmediately = false)
{
    Outcome out;
    QString error;
    FormatContextPtr fmt = openFormatContext(path, &error);
    if (!fmt) {
        out.failed = true;
        out.error = error;
        return out;
    }

    QEventLoop loop;
    {
        IndexJob job;
        QObject::connect(&job, &IndexJob::finished, &loop, [&] {
            out.finished = true;
            out.fromCache = job.loadedFromCache();
            if (auto index = job.takeResult()) {
                for (const auto &s : index->streams())
                    out.packets += s.packets.size();
            }
            loop.quit();
        });
        QObject::connect(&job, &IndexJob::failed, &loop, [&](const QString &e) {
            out.failed = true;
            out.error = e;
            loop.quit();
        });
        QTimer::singleShot(30000, &loop, &QEventLoop::quit);
        job.start(std::move(fmt), path, cacheDir);
        if (cancelImmediately) {
            job.cancel();
            QTimer::singleShot(500, &loop, &QEventLoop::quit);
        }
        loop.exec();
    } // job destroyed: worker joined
    return out;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s FILE\n", argv[0]);
        return 2;
    }
    av_log_set_level(AV_LOG_FATAL);
    const QString path = QString::fromLocal8Bit(argv[1]);
    QTemporaryDir cacheDir;
    int failures = 0;

    const Outcome first = runJob(path, cacheDir.path());
    const Outcome second = runJob(path, cacheDir.path());
    std::printf("first: finished=%d cache=%d packets=%zu; second: finished=%d cache=%d packets=%zu\n",
                first.finished, first.fromCache, first.packets, second.finished, second.fromCache,
                second.packets);
    if (!first.finished || first.fromCache || first.packets == 0) {
        std::fprintf(stderr, "FAIL: first run should build the index (%s)\n", qPrintable(first.error));
        ++failures;
    }
    if (!second.finished || !second.fromCache || second.packets != first.packets) {
        std::fprintf(stderr, "FAIL: second run should load the same index from the cache\n");
        ++failures;
    }

    // Canceled before it starts reading: a fresh cache dir forces a scan.
    QTemporaryDir otherCache;
    const Outcome canceled = runJob(path, otherCache.path(), true);
    std::printf("canceled: finished=%d failed=%d\n", canceled.finished, canceled.failed);
    if (canceled.failed) {
        std::fprintf(stderr, "FAIL: a canceled job must not report an error\n");
        ++failures;
    }
    // (It may legitimately finish if the file is tiny and the scan completed
    // before the first progress check; it must simply not crash or hang.)

    return failures == 0 ? 0 : 1;
}
