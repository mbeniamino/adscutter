#pragma once

#include "media/FFmpegUtil.h"
#include "media/PacketIndex.h"

#include <memory>
#include <vector>

struct SwrContext;

namespace adscutter {

// Decodes one audio stream from any position, using the packet index, and
// converts it to interleaved float samples at a fixed rate and channel count
// (for playback).
class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();
    AudioDecoder(const AudioDecoder &) = delete;
    AudioDecoder &operator=(const AudioDecoder &) = delete;

    bool open(const QString &path, int streamIndex, std::shared_ptr<const PacketIndex> index, int outRate,
              int outChannels, QString *error);

    // Positions the decoder so that the next samples start exactly at the
    // given unified time.
    bool seek(int64_t timeUs, QString *error);

    // Decodes the next chunk. `startUs` receives the unified time of its
    // first sample. Returns false at the end of the stream or on error
    // (then *error is set).
    bool read(std::vector<float> &samples, int64_t *startUs, QString *error);

    int outRate() const { return m_outRate; }
    int outChannels() const { return m_outChannels; }

private:
    bool feedPacket(QString *error);
    bool convert(const AVFrame *frame, std::vector<float> &samples, int64_t *startUs);

    std::shared_ptr<const PacketIndex> m_index;
    const PacketIndex::Stream *m_stream = nullptr;
    FormatContextPtr m_fmt;
    AVStream *m_avStream = nullptr;
    CodecContextPtr m_codec;
    SwrContext *m_swr = nullptr;
    AVChannelLayout m_swrInLayout{};
    int m_swrInRate = 0;
    int m_swrInFormat = -1;
    PacketPtr m_pkt;
    FramePtr m_frame;
    int m_outRate = 48000;
    int m_outChannels = 2;
    bool m_byteSeek = false;

    uint32_t m_nextPacket = 0;
    bool m_synced = false;
    bool m_inputDone = false;
    int64_t m_trimBeforeUs = INT64_MIN; // drop samples before this time
};

} // namespace adscutter
