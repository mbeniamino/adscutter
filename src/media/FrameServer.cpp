#include "media/FrameServer.h"
#include "media/Deinterlace.h"
#include "media/FrameConverter.h"
#include "media/RandomAccessDecoder.h"

extern "C" {
#include <libavutil/pixdesc.h>
}

namespace adscutter {

FrameServer::FrameServer(QObject *parent)
    : QObject(parent)
{
    static const int registered = qRegisterMetaType<adscutter::DisplayFrame>();
    Q_UNUSED(registered);
}

FrameServer::~FrameServer()
{
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

bool FrameServer::open(const QString &path, int streamIndex, std::shared_ptr<const PacketIndex> index,
                       QString *error, HwPreference hardware)
{
    Q_ASSERT(!m_thread.joinable());
    auto decoder = std::make_unique<RandomAccessDecoder>();
    if (!decoder->open(path, streamIndex, std::move(index), error, hardware))
        return false;
    m_decoder = std::move(decoder);
    m_thread = std::thread([this] { run(); });
    return true;
}

void FrameServer::request(uint32_t pos)
{
    {
        std::lock_guard lock(m_mutex);
        m_pending = pos;
        m_last = pos;
    }
    m_wake.notify_one();
}

void FrameServer::setDeinterlace(bool enabled)
{
    {
        std::lock_guard lock(m_mutex);
        if (m_deinterlace == enabled)
            return;
        m_deinterlace = enabled;
        if (m_last)
            m_pending = m_last;
    }
    m_wake.notify_one();
}

void FrameServer::setPlaying(bool playing)
{
    {
        std::lock_guard lock(m_mutex);
        m_playing = playing;
    }
    m_wake.notify_one();
}

void FrameServer::run()
{
    constexpr uint32_t kPrefetch = 12; // frames decoded ahead while playing
    FrameConverter converter;
    uint32_t prefetchNext = 0, prefetchEnd = 0;
    for (;;) {
        uint32_t pos;
        bool deinterlace;
        {
            std::unique_lock lock(m_mutex);
            auto prefetching = [&] { return m_playing && prefetchNext < prefetchEnd; };
            m_wake.wait(lock, [&] { return m_stop || m_pending || prefetching(); });
            if (m_stop)
                return;
            if (!m_pending) {
                // Nothing requested: decode one frame ahead, then check again.
                const uint32_t ahead = prefetchNext++;
                lock.unlock();
                QString ignored;
                if (!m_decoder->isCached(ahead))
                    m_decoder->frameAt(ahead, &ignored);
                continue;
            }
            pos = *m_pending;
            m_pending.reset();
            deinterlace = m_deinterlace;
        }
        prefetchNext = pos + 1;
        prefetchEnd = std::min(pos + 1 + kPrefetch, m_decoder->frameCount());

        QString error;
        FramePtr frame = m_decoder->frameAt(pos, &error);
        if (!frame) {
            emit frameFailed(pos, error);
            continue;
        }

        DisplayFrame out;
        out.pos = pos;
        out.timeUs = m_decoder->index().frameTime(m_decoder->stream(), pos);
        out.pictureType = av_get_picture_type_char(frame->pict_type);
        const auto &s = m_decoder->stream();
        out.cutPoint = s.isCutPoint(s.packets[s.displayOrder[pos]]);
        out.interlaced = frame->flags & AV_FRAME_FLAG_INTERLACED;
        out.displayAspect = displayAspect(frame.get(), m_decoder->sampleAspect());
        out.decoder = m_decoder->decoderName();

        const AVFrame *shown = frame.get();
        FramePtr progressive;
        if (deinterlace && out.interlaced) {
            progressive = deinterlaceFrame(frame.get());
            if (progressive) {
                shown = progressive.get();
                out.deinterlaced = true;
            }
        }
        out.image = converter.toImage(shown);
        if (out.image.isNull()) {
            emit frameFailed(pos, QStringLiteral("Cannot convert frame"));
            continue;
        }
        emit frameReady(out);
    }
}

} // namespace adscutter
