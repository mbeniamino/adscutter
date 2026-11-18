#pragma once

#include "media/PacketIndex.h"

#include <QObject>
#include <atomic>
#include <memory>
#include <thread>

class QAudioSink;

namespace adscutter {

class AudioDecoder;
class PcmRingDevice;

// Plays one audio stream of the recording from a given time.
//
// A feeder thread decodes ahead into a ring buffer; a pull-mode QAudioSink
// reads from it (silence on underrun, so the device never stalls).
class AudioOutput : public QObject {
    Q_OBJECT
public:
    explicit AudioOutput(QObject *parent = nullptr);
    ~AudioOutput() override;

    // False when there is no audio device (playback then runs silently).
    static bool deviceAvailable();

    bool start(const QString &path, int streamIndex, std::shared_ptr<const PacketIndex> index, int64_t fromUs,
               QString *error);
    void stop();
    bool isPlaying() const { return m_sink != nullptr; }
    // Estimated time between handing samples to the device and hearing them.
    int64_t latencyUs() const;
    void setVolume(float volume); // 0..1, kept across start()
    // Audio consumed by the device since start(), for diagnostics.
    int64_t processedUs() const;

private:
    void feed();

    std::unique_ptr<AudioDecoder> m_decoder;
    std::unique_ptr<PcmRingDevice> m_device;
    QAudioSink *m_sink = nullptr;
    std::thread m_feeder;
    std::atomic<bool> m_stop{false};
    float m_volume = 1.0f;
    int m_rate = 48000;
    int m_channels = 2;
};

} // namespace adscutter
