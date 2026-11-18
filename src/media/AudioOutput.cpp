#include "media/AudioOutput.h"
#include "media/AudioDecoder.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <limits>

namespace adscutter {

// Interleaved float samples between the feeder thread and the audio sink.
class PcmRingDevice : public QIODevice {
public:
    explicit PcmRingDevice(size_t capacitySamples)
        : m_capacity(capacitySamples)
    {
    }

    // Blocks (politely) while the buffer is full; false once closed.
    bool push(const float *data, size_t count, const std::atomic<bool> &stop)
    {
        while (count > 0) {
            std::unique_lock lock(m_mutex);
            m_space.wait(lock, [&] { return stop || m_buffer.size() < m_capacity; });
            if (stop)
                return false;
            const size_t n = std::min(count, m_capacity - m_buffer.size());
            m_buffer.insert(m_buffer.end(), data, data + n);
            data += n;
            count -= n;
        }
        return true;
    }

    void wakeAll() { m_space.notify_all(); }

    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return std::numeric_limits<qint32>::max(); }

protected:
    qint64 readData(char *out, qint64 maxSize) override
    {
        const size_t wanted = size_t(maxSize) / sizeof(float);
        size_t copied = 0;
        {
            std::lock_guard lock(m_mutex);
            copied = std::min(wanted, m_buffer.size());
            auto *dst = reinterpret_cast<float *>(out);
            std::copy(m_buffer.begin(), m_buffer.begin() + ptrdiff_t(copied), dst);
            m_buffer.erase(m_buffer.begin(), m_buffer.begin() + ptrdiff_t(copied));
        }
        // Underrun: play silence rather than letting the sink stop.
        std::memset(out + copied * sizeof(float), 0, (wanted - copied) * sizeof(float));
        m_space.notify_all();
        return qint64(wanted * sizeof(float));
    }
    qint64 writeData(const char *, qint64) override { return -1; }

private:
    std::mutex m_mutex;
    std::condition_variable m_space;
    std::deque<float> m_buffer;
    size_t m_capacity;
};

AudioOutput::AudioOutput(QObject *parent)
    : QObject(parent)
{
}

AudioOutput::~AudioOutput()
{
    stop();
}

bool AudioOutput::deviceAvailable()
{
    return !QMediaDevices::defaultAudioOutput().isNull();
}

bool AudioOutput::start(const QString &path, int streamIndex, std::shared_ptr<const PacketIndex> index,
                        int64_t fromUs, QString *error)
{
    stop();
    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (device.isNull()) {
        if (error)
            *error = QStringLiteral("No audio output device");
        return false;
    }
    QAudioFormat format;
    format.setSampleRate(48000);
    format.setChannelCount(2);
    format.setSampleFormat(QAudioFormat::Float);
    if (!device.isFormatSupported(format)) {
        format = device.preferredFormat();
        format.setSampleFormat(QAudioFormat::Float);
    }
    if (!device.isFormatSupported(format)) {
        if (error)
            *error = QStringLiteral("The audio device does not support float samples");
        return false;
    }
    m_rate = format.sampleRate();
    m_channels = format.channelCount();

    m_decoder = std::make_unique<AudioDecoder>();
    if (!m_decoder->open(path, streamIndex, std::move(index), m_rate, m_channels, error)
        || !m_decoder->seek(fromUs, error)) {
        m_decoder.reset();
        return false;
    }

    // Half a second of decoded audio ahead of the device.
    m_device = std::make_unique<PcmRingDevice>(size_t(m_rate) * size_t(m_channels) / 2);
    m_device->open(QIODevice::ReadOnly);
    m_stop = false;
    m_feeder = std::thread([this] { feed(); });

    m_sink = new QAudioSink(device, format, this);
    m_sink->setBufferSize(m_rate * m_channels * int(sizeof(float)) / 10); // 100 ms
    m_sink->setVolume(m_volume);
    m_sink->start(m_device.get());
    return true;
}

void AudioOutput::stop()
{
    m_stop = true;
    if (m_device)
        m_device->wakeAll();
    if (m_feeder.joinable())
        m_feeder.join();
    if (m_sink) {
        m_sink->stop();
        delete m_sink;
        m_sink = nullptr;
    }
    m_device.reset();
    m_decoder.reset();
}

int64_t AudioOutput::latencyUs() const
{
    if (!m_sink)
        return 0;
    return int64_t(m_sink->bufferSize()) * AV_TIME_BASE / (int64_t(m_rate) * m_channels * int64_t(sizeof(float)));
}

void AudioOutput::setVolume(float volume)
{
    m_volume = volume;
    if (m_sink)
        m_sink->setVolume(volume);
}

int64_t AudioOutput::processedUs() const
{
    return m_sink ? m_sink->processedUSecs() : 0;
}

void AudioOutput::feed()
{
    std::vector<float> samples;
    int64_t start = 0;
    QString error;
    while (!m_stop && m_decoder->read(samples, &start, &error)) {
        if (!m_device->push(samples.data(), samples.size(), m_stop))
            break;
    }
}

} // namespace adscutter
