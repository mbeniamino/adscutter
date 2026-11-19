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
// It can also play a list of ranges one after the other, skipping what is
// in between: the kept parts of the cut list ("kept parts only", as the
// exported file will be) or a few seconds around each cue or cut ("smart
// play", to check them quickly).
class PlaybackController : public QObject {
    Q_OBJECT
public:
    struct Range {
        uint32_t first, last; // frames played (inclusive)
        uint32_t anchor;      // the frame this range is about (a cue, a cut)
        QString label;
    };

    explicit PlaybackController(QObject *parent = nullptr);
    ~PlaybackController() override;

    void setSource(const QString &path, std::shared_ptr<const PacketIndex> index, int videoStream);
    void clear();
    void setAudioStream(int stream) { m_audioStream = stream; } // -1: silent
    void setKeptOnly(bool keptOnly, const CutList *cutList);
    void setMuted(bool muted);

    bool isPlaying() const { return m_timer.isActive(); }
    void play(uint32_t fromPos);
    // Plays the ranges in order (from the first one not entirely before
    // fromPos), then stops.
    void playRanges(std::vector<Range> ranges, uint32_t fromPos);
    bool isPlayingRanges() const { return isPlaying() && m_range >= 0; }
    // The range being played (valid while isPlayingRanges()).
    const Range &currentRange() const { return m_ranges[size_t(m_range)]; }
    void stop();

signals:
    void positionChanged(quint32 pos); // the frame that should be shown now
    void rangeStarted(int index, int count, const QString &label);
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
    std::vector<Range> m_ranges;
    int m_range = -1; // range being played, -1: playing straight on
    void startRange(int index, uint32_t from);
};

} // namespace adscutter
