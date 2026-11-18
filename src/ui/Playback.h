#pragma once

#include "media/PacketIndex.h"

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <memory>

namespace adscutter {

class AudioOutput;
class CutList;

// Real-time playback: a clock started with the audio decides which frame
// should be on screen; the frame server shows it (frames that cannot be
// decoded in time are skipped, never delayed).
//
// With "kept parts only" it plays the segments of the cut list back to
// back, as the exported file will be.
class PlaybackController : public QObject {
    Q_OBJECT
public:
    explicit PlaybackController(QObject *parent = nullptr);
    ~PlaybackController() override;

    void setSource(const QString &path, std::shared_ptr<const PacketIndex> index, int videoStream);
    void clear();
    void setAudioStream(int stream) { m_audioStream = stream; } // -1: silent
    void setKeptOnly(bool keptOnly, const CutList *cutList);
    void setMuted(bool muted);

    bool isPlaying() const { return m_timer.isActive(); }
    void play(uint32_t fromPos);
    void stop();

signals:
    void positionChanged(quint32 pos); // the frame that should be shown now
    void stateChanged(bool playing);
    void message(const QString &text);

private:
    void startAt(uint32_t pos);
    void tick();
    const PacketIndex::Stream *video() const;

    QString m_path;
    std::shared_ptr<const PacketIndex> m_index;
    int m_videoStream = -1;
    int m_audioStream = -1;
    bool m_keptOnly = false;
    bool m_muted = false;
    const CutList *m_cutList = nullptr;

    std::unique_ptr<AudioOutput> m_audio;
    QTimer m_timer;
    QElapsedTimer m_clock;
    int64_t m_startUs = 0;   // unified time where the clock started
    int64_t m_latencyUs = 0; // audio output latency the picture waits for
    uint32_t m_lastPos = 0;
    int m_segment = -1;      // current segment in kept-only mode
};

} // namespace adscutter
