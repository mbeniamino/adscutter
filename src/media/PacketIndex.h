#pragma once

#include "media/FFmpegUtil.h"

#include <QString>
#include <cstdint>
#include <vector>

namespace adscutter {

// Identifies the exact version of a media file an index was built from.
struct FileKey {
    int64_t size = -1;
    int64_t mtimeMs = -1;

    static FileKey of(const QString &path);
    bool operator==(const FileKey &) const = default;
};

// Per-packet index of a whole media file, built by one demux pass.
//
// Timestamps are kept in two forms:
//  - Packet::pts/dts: as read by the demuxer, in the stream time base, with
//    33-bit MPEG-TS wraparounds already removed (so they only ever increase
//    by large amounts at real discontinuities).
//  - "unified" time: microseconds on a single timeline shared by all streams,
//    starting at 0, continuous across stream discontinuities (e.g. a TV
//    channel restarting its encoder in the middle of a recording).
class PacketIndex {
public:
    enum Flag : uint16_t {
        Key = 1 << 0,          // keyframe flag set by the demuxer/parser
        RandomAccess = 1 << 1, // decoding can start here (see Bitstream.h)
        Intra = 1 << 2,        // intra coded picture
        ParamSets = 1 << 3,    // carries SPS/PPS/sequence header
        Corrupt = 1 << 4,
    };

    struct Packet {
        int64_t pos;   // byte position in the file, -1 if unknown
        int64_t pts;   // stream time base, wrap-corrected; AV_NOPTS_VALUE if missing
        int64_t dts;   // idem
        int32_t size;
        uint16_t flags;
        uint16_t segment; // index into Stream::segments
    };
    static_assert(sizeof(Packet) == 32);

    // A run of packets sharing the same offset to the unified timeline.
    struct Segment {
        uint32_t firstPacket;
        int64_t offsetUs; // added to the rescaled timestamp
    };

    struct Stream {
        int index = -1;
        AVMediaType type = AVMEDIA_TYPE_UNKNOWN;
        AVCodecID codecId = AV_CODEC_ID_NONE;
        AVRational timeBase{1, 90000};
        // No real random access points were found: plain intra frames are
        // used as cut points instead (some broadcasters send neither IDR
        // frames nor recovery point SEI).
        bool intraFallback = false;
        std::vector<Packet> packets; // in file (decode) order
        std::vector<Segment> segments;

        // Derived by finalize(), not stored in the cache:
        std::vector<uint32_t> displayOrder; // packet indices sorted by presentation time
        std::vector<uint32_t> keyframes;    // positions in displayOrder that are cut points

        bool isCutPoint(const Packet &p) const
        {
            return (p.flags & RandomAccess) || (intraFallback && (p.flags & Intra));
        }
    };

    std::vector<Stream> &streams() { return m_streams; }
    const std::vector<Stream> &streams() const { return m_streams; }
    const Stream *stream(int index) const;

    // Presentation time of a packet on the unified timeline, in microseconds,
    // or AV_NOPTS_VALUE.
    int64_t unifiedPts(const Stream &s, uint32_t packet) const;
    // Unified end time of the last presented packet of all streams.
    int64_t durationUs() const { return m_durationUs; }
    size_t discontinuities() const;

    // Computes the derived data. Must be called after building or loading.
    void finalize();

    // Cache file I/O. The key must match on load, otherwise it fails.
    bool save(const QString &path, const FileKey &key, QString *error) const;
    static std::unique_ptr<PacketIndex> load(const QString &path, const FileKey &key, QString *error);
    // ~/.cache/adscutter/<hash of absolute path>.idx (or below cacheDir)
    static QString cachePath(const QString &mediaPath, const QString &cacheDir = {});

private:
    int64_t rawUnified(const Stream &s, const Packet &p) const;

    std::vector<Stream> m_streams;
    int64_t m_originUs = 0;
    int64_t m_durationUs = 0;
};

} // namespace adscutter
