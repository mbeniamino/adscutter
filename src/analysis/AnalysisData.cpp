#include "analysis/AnalysisData.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <algorithm>
#include <cstring>

namespace adscutter {

namespace {

constexpr char kMagic[8] = {'A', 'D', 'S', 'C', 'A', 'N', 'A', '\0'};
constexpr uint32_t kVersion = 1;

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

bool AnalysisData::isComplete() const
{
    return !frames.empty() && std::all_of(frames.begin(), frames.end(), [](const FrameFeatures &f) { return f.valid; });
}

uint8_t AnalysisData::loudnessAt(int64_t timeUs) const
{
    if (timeUs < audioStartUs)
        return kNoAudio;
    const auto window = size_t((timeUs - audioStartUs) / kAudioWindowUs);
    return window < loudness.size() ? loudness[window] : kNoAudio;
}

QString AnalysisData::cachePath(const QString &mediaPath, const QString &cacheDir)
{
    QString path = PacketIndex::cachePath(mediaPath, cacheDir);
    path.chop(4); // ".idx"
    return path + QStringLiteral(".analysis");
}

bool AnalysisData::save(const QString &path, const FileKey &key, QString *error) const
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    static_assert(sizeof(FrameFeatures) == 4);
    bool ok = f.write(kMagic, sizeof(kMagic)) == qint64(sizeof(kMagic)) && writePod(f, kVersion)
              && writePod(f, key.size) && writePod(f, key.mtimeMs) && writePod(f, int32_t(videoStream))
              && writePod(f, int32_t(audioStream)) && writePod(f, uint64_t(frames.size()))
              && writePod(f, audioStartUs) && writePod(f, uint64_t(loudness.size()));
    ok = ok && f.write(reinterpret_cast<const char *>(frames.data()), qint64(frames.size() * sizeof(FrameFeatures)))
                   == qint64(frames.size() * sizeof(FrameFeatures));
    ok = ok && f.write(reinterpret_cast<const char *>(loudness.data()), qint64(loudness.size())) == qint64(loudness.size());
    if (!ok || !f.commit()) {
        if (error)
            *error = f.errorString();
        f.cancelWriting();
        return false;
    }
    return true;
}

std::unique_ptr<AnalysisData> AnalysisData::load(const QString &path, const FileKey &key, QString *error)
{
    auto fail = [&](const QString &why) -> std::unique_ptr<AnalysisData> {
        if (error)
            *error = why;
        return nullptr;
    };
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return fail(f.errorString());
    char magic[sizeof(kMagic)];
    uint32_t version = 0;
    FileKey stored;
    int32_t video = -1, audio = -1;
    uint64_t frameCount = 0, windows = 0;
    auto data = std::make_unique<AnalysisData>();
    if (f.read(magic, sizeof(magic)) != qint64(sizeof(magic)) || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0
        || !readPod(f, version) || version != kVersion)
        return fail(QStringLiteral("not an analysis file of this version"));
    if (!readPod(f, stored.size) || !readPod(f, stored.mtimeMs) || !(stored == key))
        return fail(QStringLiteral("recording changed since it was analysed"));
    if (!readPod(f, video) || !readPod(f, audio) || !readPod(f, frameCount) || !readPod(f, data->audioStartUs)
        || !readPod(f, windows) || frameCount * sizeof(FrameFeatures) + windows != uint64_t(f.size() - f.pos()))
        return fail(QStringLiteral("corrupt analysis file"));
    data->videoStream = video;
    data->audioStream = audio;
    data->frames.resize(frameCount);
    data->loudness.resize(windows);
    f.read(reinterpret_cast<char *>(data->frames.data()), qint64(frameCount * sizeof(FrameFeatures)));
    f.read(reinterpret_cast<char *>(data->loudness.data()), qint64(windows));
    return data;
}

std::vector<std::pair<int64_t, int64_t>> findSilences(const AnalysisData &data, const DetectionSettings &settings)
{
    std::vector<std::pair<int64_t, int64_t>> out;
    size_t i = 0;
    while (i < data.loudness.size()) {
        const uint8_t l = data.loudness[i];
        if (l == AnalysisData::kNoAudio || l < settings.silenceDb) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < data.loudness.size() && data.loudness[j] != AnalysisData::kNoAudio
               && data.loudness[j] >= settings.silenceDb)
            ++j;
        const int64_t start = data.audioStartUs + int64_t(i) * AnalysisData::kAudioWindowUs;
        const int64_t end = data.audioStartUs + int64_t(j) * AnalysisData::kAudioWindowUs;
        if (end - start >= settings.minSilenceUs)
            out.emplace_back(start, end);
        i = j;
    }
    return out;
}

std::vector<BreakCandidate> findBreakCandidates(const AnalysisData &data, const PacketIndex &index,
                                                const DetectionSettings &settings)
{
    std::vector<BreakCandidate> out;
    const PacketIndex::Stream *video = index.stream(data.videoStream);
    if (!video || data.frames.size() != video->displayOrder.size())
        return out;

    // Dark runs, merging those separated by a few frames (flashes, noise).
    for (uint32_t pos = 0; pos < data.frames.size(); ++pos) {
        const FrameFeatures &f = data.frames[pos];
        if (!f.valid || f.dark < settings.darkFraction)
            continue;
        if (!out.empty() && pos <= out.back().last + settings.mergeGapFrames + 1)
            out.back().last = pos;
        else
            out.push_back({pos, pos, false});
    }

    // Did the picture fade into it? Brightness before the dark frames falls
    // by a clear amount, and (nearly) every step goes down.
    for (BreakCandidate &c : out) {
        if (c.first == 0)
            continue;
        const int64_t end = index.frameTime(*video, c.first);
        uint32_t start = c.first;
        while (start > 0 && end - index.frameTime(*video, start - 1) <= settings.fadeWindowUs)
            --start;
        if (c.first - start < 4)
            continue;
        int down = 0, steps = 0;
        for (uint32_t f = start + 1; f < c.first; ++f, ++steps)
            down += data.frames[f].mean <= data.frames[f - 1].mean + 1;
        const int drop = int(data.frames[start].mean) - int(data.frames[c.first - 1].mean);
        c.fade = drop >= settings.fadeDrop && down * 10 >= steps * 7;
    }

    // Silence at (about) the same time makes a candidate strong.
    const auto silences = findSilences(data, settings);
    for (BreakCandidate &c : out) {
        const int64_t from = index.frameTime(*video, c.first) - settings.silenceToleranceUs;
        const int64_t to = index.frameTime(*video, c.last) + settings.silenceToleranceUs;
        auto it = std::lower_bound(silences.begin(), silences.end(), from,
                                   [](const auto &s, int64_t t) { return s.second < t; });
        c.silent = it != silences.end() && it->first <= to;
    }
    return out;
}

std::vector<BreakRegion> findBreaks(const std::vector<BreakCandidate> &candidates, const PacketIndex &index,
                                    int videoStream, const DetectionSettings &settings)
{
    std::vector<BreakRegion> out;
    const PacketIndex::Stream *video = index.stream(videoStream);
    if (!video)
        return out;
    BreakRegion current{};
    int64_t lastEndUs = INT64_MIN;
    auto flush = [&] {
        if (current.candidates >= settings.minBreakCandidates)
            out.push_back(current);
        current = {};
    };
    for (const BreakCandidate &c : candidates) {
        if (!c.silent)
            continue;
        const int64_t start = index.frameTime(*video, c.first);
        if (current.candidates > 0 && start - lastEndUs > settings.maxSpotUs)
            flush();
        if (current.candidates == 0)
            current.first = c.first;
        current.last = c.last;
        ++current.candidates;
        lastEndUs = index.frameTime(*video, c.last);
    }
    flush();

    // Extend each break back to the fade that started it.
    uint32_t previousEnd = 0;
    for (BreakRegion &br : out) {
        const int64_t breakStart = index.frameTime(*video, br.first);
        for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
            if (it->first >= br.first || it->first <= previousEnd)
                continue;
            if (breakStart - index.frameTime(*video, it->first) > settings.fadeLeadUs)
                break;
            if (it->fade) {
                br.first = it->first;
                br.startsWithFade = true;
                break;
            }
        }
        previousEnd = br.last;
    }
    return out;
}

} // namespace adscutter
