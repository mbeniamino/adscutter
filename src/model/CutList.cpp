#include "model/CutList.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <algorithm>

namespace adscutter {

namespace {
constexpr size_t kMaxUndo = 200;
constexpr int kFormatVersion = 1;
} // namespace

void CutList::setFrames(uint32_t frameCount, std::vector<uint32_t> keyframes)
{
    m_frameCount = frameCount;
    std::sort(keyframes.begin(), keyframes.end());
    keyframes.erase(std::unique(keyframes.begin(), keyframes.end()), keyframes.end());
    if (keyframes.empty() || keyframes.front() != 0)
        keyframes.insert(keyframes.begin(), 0);
    m_keyframes = std::move(keyframes);
    m_segments.clear();
    m_undo.clear();
    m_redo.clear();
    clearMarks();
    m_revision = m_savedRevision = ++m_revisionCounter;
}

uint32_t CutList::snapIn(uint32_t pos) const
{
    auto it = std::upper_bound(m_keyframes.begin(), m_keyframes.end(), pos);
    return it == m_keyframes.begin() ? 0 : *(it - 1);
}

uint32_t CutList::keyframeAtOrAfter(uint32_t pos) const
{
    auto it = std::lower_bound(m_keyframes.begin(), m_keyframes.end(), pos);
    return it == m_keyframes.end() ? UINT32_MAX : *it;
}

void CutList::setSegments(const std::vector<Segment> &segments)
{
    std::vector<Segment> old;
    old.swap(m_segments);
    for (const Segment &s : segments)
        addSegment(s.in, s.out);
    if (m_segments != old)
        changed();
}

CutList::MarkResult CutList::markIn(uint32_t pos)
{
    if (pos >= m_frameCount)
        return MarkResult::Invalid;
    m_pendingIn = snapIn(pos);
    if (m_pendingOut) {
        if (*m_pendingOut < *m_pendingIn) {
            m_pendingOut.reset(); // the new in replaces the stale out
            return MarkResult::Invalid;
        }
        checkpoint();
        addSegment(*m_pendingIn, *m_pendingOut);
        clearMarks();
        return MarkResult::Added;
    }
    return MarkResult::Pending;
}

CutList::MarkResult CutList::markOut(uint32_t pos)
{
    if (pos >= m_frameCount)
        return MarkResult::Invalid;
    m_pendingOut = pos;
    if (m_pendingIn) {
        if (pos < *m_pendingIn) {
            m_pendingIn.reset(); // the new out replaces the stale in
            return MarkResult::Invalid;
        }
        checkpoint();
        addSegment(*m_pendingIn, pos);
        clearMarks();
        return MarkResult::Added;
    }
    return MarkResult::Pending;
}

void CutList::clearMarks()
{
    m_pendingIn.reset();
    m_pendingOut.reset();
}

bool CutList::addSegment(uint32_t in, uint32_t out)
{
    if (m_frameCount == 0)
        return false;
    out = std::min(out, m_frameCount - 1);
    in = snapIn(std::min(in, out));

    // Merge with every segment overlapping or touching [in, out].
    std::vector<Segment> result;
    result.reserve(m_segments.size() + 1);
    Segment merged{in, out};
    for (const Segment &s : m_segments) {
        if (uint64_t(s.out) + 1 < merged.in || uint64_t(merged.out) + 1 < s.in) {
            result.push_back(s);
        } else {
            merged.in = std::min(merged.in, s.in);
            merged.out = std::max(merged.out, s.out);
        }
    }
    result.push_back(merged);
    std::sort(result.begin(), result.end(), [](const Segment &a, const Segment &b) { return a.in < b.in; });
    if (result == m_segments)
        return false;
    m_segments = std::move(result);
    changed();
    return true;
}

bool CutList::removeSegment(size_t index)
{
    if (index >= m_segments.size())
        return false;
    m_segments.erase(m_segments.begin() + ptrdiff_t(index));
    changed();
    return true;
}

void CutList::clear()
{
    if (m_segments.empty())
        return;
    m_segments.clear();
    changed();
}

int CutList::segmentAt(uint32_t pos) const
{
    auto it = std::upper_bound(m_segments.begin(), m_segments.end(), pos,
                               [](uint32_t p, const Segment &s) { return p < s.in; });
    if (it == m_segments.begin())
        return -1;
    --it;
    return pos <= it->out ? int(it - m_segments.begin()) : -1;
}

uint32_t CutList::moveIn(size_t index, uint32_t pos)
{
    if (index >= m_segments.size())
        return 0;
    Segment &s = m_segments[index];
    const uint32_t lower = index > 0 ? m_segments[index - 1].out + 1 : 0;

    uint32_t in = snapIn(std::min(pos, s.out));
    if (in < lower) {
        // The cut point before the frame is inside the previous segment:
        // use the first one after it instead, if the segment stays non-empty.
        const uint32_t next = keyframeAtOrAfter(lower);
        in = next <= s.out ? next : s.in;
    }
    if (in != s.in) {
        s.in = in;
        changed();
    }
    return s.in;
}

uint32_t CutList::moveOut(size_t index, uint32_t pos)
{
    if (index >= m_segments.size())
        return 0;
    Segment &s = m_segments[index];
    const uint32_t upper = index + 1 < m_segments.size() ? m_segments[index + 1].in - 1 : m_frameCount - 1;
    const uint32_t out = std::clamp(pos, s.in, upper);
    if (out != s.out) {
        s.out = out;
        changed();
    }
    return s.out;
}

uint64_t CutList::keptFrames() const
{
    uint64_t n = 0;
    for (const Segment &s : m_segments)
        n += uint64_t(s.out) - s.in + 1;
    return n;
}

std::vector<uint32_t> CutList::boundaries() const
{
    std::vector<uint32_t> b;
    for (const Segment &s : m_segments) {
        b.push_back(s.in);
        b.push_back(s.out);
    }
    if (m_pendingIn)
        b.push_back(*m_pendingIn);
    if (m_pendingOut)
        b.push_back(*m_pendingOut);
    std::sort(b.begin(), b.end());
    b.erase(std::unique(b.begin(), b.end()), b.end());
    return b;
}

void CutList::checkpoint()
{
    m_undo.push_back({m_segments, m_revision});
    if (m_undo.size() > kMaxUndo)
        m_undo.erase(m_undo.begin());
    m_redo.clear();
}

bool CutList::undo()
{
    // Skip checkpoints that recorded no actual change (e.g. a click on an
    // edge without moving it).
    while (!m_undo.empty()) {
        State state = std::move(m_undo.back());
        m_undo.pop_back();
        if (state.segments == m_segments && state.revision == m_revision)
            continue;
        m_redo.push_back({m_segments, m_revision});
        m_segments = std::move(state.segments);
        m_revision = state.revision;
        return true;
    }
    return false;
}

bool CutList::redo()
{
    if (m_redo.empty())
        return false;
    m_undo.push_back({m_segments, m_revision});
    m_segments = std::move(m_redo.back().segments);
    m_revision = m_redo.back().revision;
    m_redo.pop_back();
    return true;
}

// --- JSON ----------------------------------------------------------------------

bool saveCutListDocument(const QString &path, const CutListDocument &doc, QString *error)
{
    QJsonArray streams;
    for (int s : doc.selectedStreams)
        streams.append(s);
    QJsonArray segments;
    for (const auto &e : doc.segments) {
        segments.append(QJsonObject{
            {QStringLiteral("inFrame"), qint64(e.frames.in)},
            {QStringLiteral("outFrame"), qint64(e.frames.out)},
            {QStringLiteral("inTimeUs"), qint64(e.inTimeUs)},
            {QStringLiteral("outTimeUs"), qint64(e.outTimeUs)},
        });
    }
    const QJsonObject root{
        {QStringLiteral("format"), QStringLiteral("adscutter-cutlist")},
        {QStringLiteral("version"), kFormatVersion},
        {QStringLiteral("media"), QJsonObject{
             {QStringLiteral("file"), doc.mediaFile},
             {QStringLiteral("size"), qint64(doc.mediaSize)},
         }},
        {QStringLiteral("videoStream"), doc.videoStream},
        {QStringLiteral("frameCount"), qint64(doc.frameCount)},
        {QStringLiteral("selectedStreams"), streams},
        {QStringLiteral("segments"), segments},
    };

    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return true;
}

std::optional<CutListDocument> loadCutListDocument(const QString &path, QString *error)
{
    auto fail = [&](const QString &why) -> std::optional<CutListDocument> {
        if (error)
            *error = why;
        return std::nullopt;
    };

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return fail(f.errorString());
    QJsonParseError parseError;
    const QJsonDocument json = QJsonDocument::fromJson(f.readAll(), &parseError);
    if (json.isNull())
        return fail(parseError.errorString());
    const QJsonObject root = json.object();
    if (root.value(QStringLiteral("format")).toString() != QStringLiteral("adscutter-cutlist"))
        return fail(QStringLiteral("Not an AdsCutter cut list"));
    if (root.value(QStringLiteral("version")).toInt() > kFormatVersion)
        return fail(QStringLiteral("Cut list written by a newer version of AdsCutter"));

    CutListDocument doc;
    const QJsonObject media = root.value(QStringLiteral("media")).toObject();
    doc.mediaFile = media.value(QStringLiteral("file")).toString();
    doc.mediaSize = media.value(QStringLiteral("size")).toInteger(-1);
    doc.videoStream = root.value(QStringLiteral("videoStream")).toInt(-1);
    doc.frameCount = uint32_t(root.value(QStringLiteral("frameCount")).toInteger(0));
    for (const QJsonValue &v : root.value(QStringLiteral("selectedStreams")).toArray())
        doc.selectedStreams.push_back(v.toInt());
    for (const QJsonValue &v : root.value(QStringLiteral("segments")).toArray()) {
        const QJsonObject o = v.toObject();
        const qint64 in = o.value(QStringLiteral("inFrame")).toInteger(-1);
        const qint64 out = o.value(QStringLiteral("outFrame")).toInteger(-1);
        if (in < 0 || out < in || out > UINT32_MAX)
            return fail(QStringLiteral("Invalid segment in cut list"));
        doc.segments.push_back({{uint32_t(in), uint32_t(out)},
                                o.value(QStringLiteral("inTimeUs")).toInteger(),
                                o.value(QStringLiteral("outTimeUs")).toInteger()});
    }
    return doc;
}

QString defaultCutListPath(const QString &mediaPath)
{
    const QFileInfo info(mediaPath);
    return info.absolutePath() + QLatin1Char('/') + info.completeBaseName() + QStringLiteral(".adscutter.json");
}

} // namespace adscutter
