#include "media/PacketIndex.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>
#include <cstring>

namespace adscutter {

namespace {

constexpr char kMagic[8] = {'A', 'D', 'S', 'C', 'I', 'D', 'X', '\0'};
constexpr uint32_t kVersion = 1;
constexpr uint32_t kByteOrderMark = 0x01020304;

template <typename T>
bool writePod(QSaveFile &f, const T &v)
{
    return f.write(reinterpret_cast<const char *>(&v), sizeof(T)) == qint64(sizeof(T));
}

template <typename T>
bool readPod(QFile &f, T &v)
{
    return f.read(reinterpret_cast<char *>(&v), sizeof(T)) == qint64(sizeof(T));
}

} // namespace

FileKey FileKey::of(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists())
        return {};
    return {info.size(), info.lastModified().toMSecsSinceEpoch()};
}

const PacketIndex::Stream *PacketIndex::stream(int index) const
{
    for (const Stream &s : m_streams) {
        if (s.index == index)
            return &s;
    }
    return nullptr;
}

int64_t PacketIndex::rawUnified(const Stream &s, const Packet &p) const
{
    if (p.pts == AV_NOPTS_VALUE)
        return AV_NOPTS_VALUE;
    return av_rescale_q(p.pts, s.timeBase, AV_TIME_BASE_Q) + s.segments[p.segment].offsetUs;
}

int64_t PacketIndex::unifiedPts(const Stream &s, uint32_t packet) const
{
    const int64_t t = rawUnified(s, s.packets[packet]);
    return t == AV_NOPTS_VALUE ? t : t - m_originUs;
}

size_t PacketIndex::discontinuities() const
{
    size_t n = 0;
    for (const Stream &s : m_streams)
        n = std::max(n, s.segments.empty() ? size_t(0) : s.segments.size() - 1);
    return n;
}

void PacketIndex::finalize()
{
    int64_t origin = INT64_MAX;
    int64_t end = INT64_MIN;

    for (Stream &s : m_streams) {
        s.displayOrder.clear();
        s.keyframes.clear();
        if (s.segments.empty())
            s.segments.push_back({0, 0});

        const bool video = s.type == AVMEDIA_TYPE_VIDEO;
        uint32_t first = 0;
        if (video) {
            bool haveRandomAccess = false, haveIntra = false;
            for (const Packet &p : s.packets) {
                haveRandomAccess |= (p.flags & RandomAccess) != 0;
                haveIntra |= (p.flags & Intra) != 0;
            }
            s.intraFallback = !haveRandomAccess && haveIntra;

            // Frames before the first cut point (a recording usually starts
            // mid-GOP) cannot be decoded, so they are not part of the
            // navigable timeline.
            first = uint32_t(s.packets.size());
            for (uint32_t i = 0; i < s.packets.size(); ++i) {
                if (s.isCutPoint(s.packets[i])) {
                    first = i;
                    break;
                }
            }
        }

        const int64_t firstPts = first < s.packets.size() ? rawUnified(s, s.packets[first]) : AV_NOPTS_VALUE;
        for (uint32_t i = first; i < s.packets.size(); ++i) {
            const int64_t t = rawUnified(s, s.packets[i]);
            if (t == AV_NOPTS_VALUE)
                continue;
            if (video && firstPts != AV_NOPTS_VALUE && t < firstPts)
                continue; // leading pictures of an open GOP referencing missing frames
            s.displayOrder.push_back(i);
        }
        std::stable_sort(s.displayOrder.begin(), s.displayOrder.end(), [&](uint32_t a, uint32_t b) {
            return rawUnified(s, s.packets[a]) < rawUnified(s, s.packets[b]);
        });

        if (video) {
            for (uint32_t pos = 0; pos < s.displayOrder.size(); ++pos) {
                if (s.isCutPoint(s.packets[s.displayOrder[pos]]))
                    s.keyframes.push_back(pos);
            }
        }

        if (s.displayOrder.empty())
            continue;
        const int64_t firstT = rawUnified(s, s.packets[s.displayOrder.front()]);
        int64_t lastT = rawUnified(s, s.packets[s.displayOrder.back()]);
        if (s.displayOrder.size() >= 2) // add one packet duration
            lastT += lastT - rawUnified(s, s.packets[s.displayOrder[s.displayOrder.size() - 2]]);
        if (s.type == AVMEDIA_TYPE_VIDEO || s.type == AVMEDIA_TYPE_AUDIO) {
            origin = std::min(origin, firstT);
            end = std::max(end, lastT);
        }
    }

    m_originUs = origin == INT64_MAX ? 0 : origin;
    m_durationUs = end == INT64_MIN ? 0 : end - m_originUs;
}

QString PacketIndex::cachePath(const QString &mediaPath, const QString &cacheDir)
{
    QString dir = cacheDir;
    if (dir.isEmpty())
        dir = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
              + QStringLiteral("/adscutter");
    const QString canonical = QFileInfo(mediaPath).absoluteFilePath();
    const QByteArray hash = QCryptographicHash::hash(canonical.toUtf8(), QCryptographicHash::Sha1).toHex();
    return dir + QLatin1Char('/') + QString::fromLatin1(hash) + QStringLiteral(".idx");
}

bool PacketIndex::save(const QString &path, const FileKey &key, QString *error) const
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }

    bool ok = f.write(kMagic, sizeof(kMagic)) == qint64(sizeof(kMagic));
    ok = ok && writePod(f, kVersion) && writePod(f, kByteOrderMark);
    ok = ok && writePod(f, key.size) && writePod(f, key.mtimeMs);
    ok = ok && writePod(f, uint32_t(m_streams.size()));
    for (const Stream &s : m_streams) {
        ok = ok && writePod(f, int32_t(s.index)) && writePod(f, int32_t(s.type))
             && writePod(f, int32_t(s.codecId)) && writePod(f, int32_t(s.timeBase.num))
             && writePod(f, int32_t(s.timeBase.den)) && writePod(f, uint32_t(s.segments.size()))
             && writePod(f, uint64_t(s.packets.size()));
        for (const Segment &seg : s.segments)
            ok = ok && writePod(f, seg.firstPacket) && writePod(f, seg.offsetUs);
        const qint64 bytes = qint64(s.packets.size() * sizeof(Packet));
        ok = ok && (bytes == 0 || f.write(reinterpret_cast<const char *>(s.packets.data()), bytes) == bytes);
    }

    if (!ok || !f.commit()) {
        if (error)
            *error = f.errorString();
        f.cancelWriting();
        return false;
    }
    return true;
}

std::unique_ptr<PacketIndex> PacketIndex::load(const QString &path, const FileKey &key, QString *error)
{
    auto fail = [&](const QString &why) -> std::unique_ptr<PacketIndex> {
        if (error)
            *error = why;
        return nullptr;
    };

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return fail(f.errorString());

    char magic[sizeof(kMagic)];
    uint32_t version = 0, bom = 0, nStreams = 0;
    FileKey stored;
    if (f.read(magic, sizeof(magic)) != qint64(sizeof(magic)) || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0)
        return fail(QStringLiteral("not an index file"));
    if (!readPod(f, version) || version != kVersion || !readPod(f, bom) || bom != kByteOrderMark)
        return fail(QStringLiteral("unsupported index version"));
    if (!readPod(f, stored.size) || !readPod(f, stored.mtimeMs) || !(stored == key))
        return fail(QStringLiteral("media file changed since it was indexed"));
    if (!readPod(f, nStreams) || nStreams > 4096)
        return fail(QStringLiteral("corrupt index"));

    auto index = std::make_unique<PacketIndex>();
    index->m_streams.resize(nStreams);
    for (Stream &s : index->m_streams) {
        int32_t idx, type, codec, tbNum, tbDen;
        uint32_t nSegments;
        uint64_t nPackets;
        if (!readPod(f, idx) || !readPod(f, type) || !readPod(f, codec) || !readPod(f, tbNum)
            || !readPod(f, tbDen) || !readPod(f, nSegments) || !readPod(f, nPackets))
            return fail(QStringLiteral("truncated index"));
        const qint64 remaining = f.size() - f.pos();
        if (tbNum <= 0 || tbDen <= 0 || nSegments > 65536
            || nPackets > uint64_t(remaining) / sizeof(Packet))
            return fail(QStringLiteral("corrupt index"));

        s.index = idx;
        s.type = AVMediaType(type);
        s.codecId = AVCodecID(codec);
        s.timeBase = {tbNum, tbDen};
        s.segments.resize(nSegments);
        for (Segment &seg : s.segments) {
            if (!readPod(f, seg.firstPacket) || !readPod(f, seg.offsetUs))
                return fail(QStringLiteral("truncated index"));
        }
        s.packets.resize(nPackets);
        const qint64 bytes = qint64(nPackets * sizeof(Packet));
        if (bytes > 0 && f.read(reinterpret_cast<char *>(s.packets.data()), bytes) != bytes)
            return fail(QStringLiteral("truncated index"));
        for (const Packet &p : s.packets) {
            if (p.segment >= std::max<size_t>(s.segments.size(), 1))
                return fail(QStringLiteral("corrupt index"));
        }
    }
    if (!f.atEnd())
        return fail(QStringLiteral("trailing data in index"));

    index->finalize();
    return index;
}

} // namespace adscutter
