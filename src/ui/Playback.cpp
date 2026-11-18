#include "ui/Playback.h"
#include "media/AudioOutput.h"
#include "model/CutList.h"

namespace adscutter {

PlaybackController::PlaybackController(QObject *parent)
    : QObject(parent)
{
    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(5);
    connect(&m_timer, &QTimer::timeout, this, &PlaybackController::tick);
}

PlaybackController::~PlaybackController()
{
    stop();
}

void PlaybackController::setSource(const QString &path, std::shared_ptr<const PacketIndex> index, int videoStream)
{
    stop();
    m_path = path;
    m_index = std::move(index);
    m_videoStream = videoStream;
}

void PlaybackController::clear()
{
    stop();
    m_index.reset();
    m_videoStream = -1;
}

void PlaybackController::setKeptOnly(bool keptOnly, const CutList *cutList)
{
    m_keptOnly = keptOnly;
    m_cutList = cutList;
}

void PlaybackController::setMuted(bool muted)
{
    m_muted = muted;
    if (m_audio)
        m_audio->setVolume(muted ? 0.0f : 1.0f);
}

const PacketIndex::Stream *PlaybackController::video() const
{
    return m_index ? m_index->stream(m_videoStream) : nullptr;
}

void PlaybackController::play(uint32_t fromPos)
{
    const PacketIndex::Stream *v = video();
    if (!v || v->displayOrder.empty())
        return;
    const uint32_t last = uint32_t(v->displayOrder.size()) - 1;
    if (fromPos >= last)
        fromPos = 0; // at the end: start over

    m_segment = -1;
    if (m_keptOnly && m_cutList && !m_cutList->isEmpty()) {
        const auto &segs = m_cutList->segments();
        // The segment containing the position, else the next one, else the first.
        m_segment = m_cutList->segmentAt(fromPos);
        if (m_segment < 0) {
            auto it = std::find_if(segs.begin(), segs.end(), [&](const Segment &s) { return s.in > fromPos; });
            m_segment = it != segs.end() ? int(it - segs.begin()) : 0;
            fromPos = segs[size_t(m_segment)].in;
        }
    } else if (m_keptOnly) {
        emit message(tr("No kept parts yet: playing everything"));
    }
    startAt(fromPos);
    m_timer.start();
    emit stateChanged(true);
}

void PlaybackController::startAt(uint32_t pos)
{
    const PacketIndex::Stream *v = video();
    m_startUs = m_index->frameTime(*v, pos);
    m_latencyUs = 0;
    m_lastPos = pos;
    if (m_audio)
        m_audio->stop();
    if (m_audioStream >= 0 && !qEnvironmentVariableIsSet("ADSCUTTER_NO_AUDIO") && AudioOutput::deviceAvailable()) {
        if (!m_audio) {
            m_audio = std::make_unique<AudioOutput>();
            m_audio->setVolume(m_muted ? 0.0f : 1.0f);
        }
        QString error;
        if (m_audio->start(m_path, m_audioStream, m_index, m_startUs, &error))
            m_latencyUs = m_audio->latencyUs();
        else
            emit message(tr("Playing without sound: %1").arg(error));
    }
    m_clock.start();
    emit positionChanged(pos);
}

void PlaybackController::stop()
{
    if (!m_timer.isActive())
        return;
    m_timer.stop();
    if (m_audio)
        m_audio->stop();
    emit stateChanged(false);
}

void PlaybackController::tick()
{
    const PacketIndex::Stream *v = video();
    if (!v) {
        stop();
        return;
    }
    // The picture waits for the sound to come out of the device.
    const int64_t now = m_startUs + std::max<int64_t>(0, m_clock.nsecsElapsed() / 1000 - m_latencyUs);
    // Last frame whose time has come.
    uint32_t pos = m_index->positionAtTime(*v, now);
    if (pos > 0 && m_index->frameTime(*v, pos) > now)
        --pos;
    pos = std::max(pos, m_lastPos); // never backwards
    const uint32_t last = uint32_t(v->displayOrder.size()) - 1;

    if (m_segment >= 0 && m_cutList) {
        const auto &segs = m_cutList->segments();
        if (size_t(m_segment) >= segs.size()) {
            stop();
            return;
        }
        if (pos > segs[size_t(m_segment)].out) {
            if (size_t(m_segment) + 1 < segs.size()) {
                ++m_segment;
                startAt(segs[size_t(m_segment)].in);
            } else {
                stop();
            }
            return;
        }
    }

    if (pos != m_lastPos) {
        m_lastPos = pos;
        emit positionChanged(pos);
    }
    if (pos >= last)
        stop();
}

} // namespace adscutter
