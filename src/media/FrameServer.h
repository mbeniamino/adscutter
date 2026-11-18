#pragma once

#include "media/PacketIndex.h"

#include <QImage>
#include <QObject>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace adscutter {

class RandomAccessDecoder;

struct DisplayFrame {
    uint32_t pos = 0;      // position in the stream's displayOrder
    int64_t timeUs = 0;    // unified time
    QImage image;          // RGB32, coded resolution
    double displayAspect = 16.0 / 9.0;
    char pictureType = '?'; // 'I', 'P', 'B', ...
    bool cutPoint = false;
    bool interlaced = false;
    bool deinterlaced = false;
};

// Serves decoded frames from a background thread. Requests are coalesced:
// when several arrive while a frame is being decoded, only the most recent
// one is served next, which keeps scrubbing and key repeat responsive.
class FrameServer : public QObject {
    Q_OBJECT
public:
    explicit FrameServer(QObject *parent = nullptr);
    ~FrameServer() override;

    bool open(const QString &path, int streamIndex, std::shared_ptr<const PacketIndex> index,
              QString *error);

    void request(uint32_t pos);
    void setDeinterlace(bool enabled); // re-serves the last requested frame
    // While playing, frames after the last requested one are decoded ahead
    // into the cache, so that the next request is served immediately.
    void setPlaying(bool playing);

signals:
    // Emitted from the worker thread.
    void frameReady(const adscutter::DisplayFrame &frame);
    void frameFailed(quint32 pos, const QString &error);

private:
    void run();

    std::unique_ptr<RandomAccessDecoder> m_decoder;
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::optional<uint32_t> m_pending;
    std::optional<uint32_t> m_last;
    bool m_deinterlace = true;
    bool m_playing = false;
    bool m_stop = false;
};

} // namespace adscutter

Q_DECLARE_METATYPE(adscutter::DisplayFrame)
