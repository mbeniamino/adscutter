#pragma once

#include "media/FFmpegUtil.h"
#include "media/PacketIndex.h"

#include <list>
#include <memory>
#include <unordered_map>

namespace adscutter {

// Frame-accurate access to the frames of one video stream, using the
// packet index.
//
// To show frame N it seeks to the cut point at or before N and decodes
// forward. Every frame decoded on the way is kept in an LRU cache of
// decoded (YUV) frames, so stepping backwards inside a GOP is instant, and
// stepping forwards continues the current decoding run without seeking.
//
// Not thread safe: use from one thread at a time.
class RandomAccessDecoder {
public:
    RandomAccessDecoder();
    ~RandomAccessDecoder();
    RandomAccessDecoder(const RandomAccessDecoder &) = delete;
    RandomAccessDecoder &operator=(const RandomAccessDecoder &) = delete;

    bool open(const QString &path, int streamIndex, std::shared_ptr<const PacketIndex> index,
              QString *error);

    const PacketIndex &index() const { return *m_index; }
    const PacketIndex::Stream &stream() const { return *m_stream; }
    uint32_t frameCount() const { return uint32_t(m_stream->displayOrder.size()); }
    AVRational sampleAspect() const { return m_sampleAspect; }

    // Returns a new reference to the frame at display position pos, or
    // nullptr on error.
    FramePtr frameAt(uint32_t pos, QString *error);

    void setCacheBudget(size_t bytes);
    size_t cachedFrames() const { return m_cache.size(); }

    // Statistics, for tests and diagnostics.
    struct Stats {
        uint64_t seeks = 0;
        uint64_t decodedFrames = 0;
        uint64_t cacheHits = 0;
    };
    const Stats &stats() const { return m_stats; }

private:
    bool startRun(uint32_t keyPos, QString *error);
    // Reads packets until one video packet was sent to the decoder.
    // Returns false on error; sets m_inputDone at the end of the file.
    bool feedPacket(QString *error);
    void store(int32_t pos, FramePtr frame);
    void evict();

    std::shared_ptr<const PacketIndex> m_index;
    const PacketIndex::Stream *m_stream = nullptr;
    FormatContextPtr m_fmt;
    AVStream *m_avStream = nullptr;
    CodecContextPtr m_codec;
    PacketPtr m_pkt;
    FramePtr m_frame;
    AVRational m_sampleAspect{0, 1};
    bool m_byteSeek = false;

    // Current decoding run: started at cut point m_runFirstPos and fed
    // sequentially from there in decode order.
    bool m_runValid = false;
    bool m_synced = false;    // the next packet read is the one at m_nextPacket
    bool m_inputDone = false; // end of file reached, decoder being drained
    uint32_t m_nextPacket = 0;
    int64_t m_runFirstPos = 0;
    int64_t m_lastOutput = -1;

    struct Entry {
        FramePtr frame;
        size_t bytes;
        std::list<uint32_t>::iterator lru;
    };
    std::unordered_map<uint32_t, Entry> m_cache;
    std::list<uint32_t> m_lru; // most recently used first
    size_t m_cacheBytes = 0;
    size_t m_cacheBudget = size_t(512) << 20;

    Stats m_stats;
};

} // namespace adscutter
