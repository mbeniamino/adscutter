#pragma once

#include "media/FFmpegUtil.h"

#include <QString>
#include <QStringList>
#include <vector>

namespace adscutter {

struct StreamInfo {
    int index = -1;
    AVMediaType type = AVMEDIA_TYPE_UNKNOWN;
    AVCodecID codecId = AV_CODEC_ID_NONE;
    QString codecName;
    QString language; // ISO 639-2 code from container metadata, if any
    QString title;
    int programId = -1; // DVB service id when the file contains several programs
    int64_t bitRate = 0;
    bool isDefault = false;
    bool hearingImpaired = false;
    bool visualImpaired = false;

    // Video
    int width = 0;
    int height = 0;
    AVRational sampleAspect{0, 1};
    AVRational frameRate{0, 1};
    AVFieldOrder fieldOrder = AV_FIELD_UNKNOWN;

    // Audio
    int sampleRate = 0;
    int channels = 0;
    QString channelLayout;

    // Whether the Matroska muxer accepts this codec with stream copy.
    bool mkvCompatible = false;
    // Human readable reason when the stream cannot be exported (yet).
    QString exportNote;

    QString typeName() const;
    QString details() const;
};

// An opened, probed input file. Owns the demuxer context.
class MediaFile {
public:
    MediaFile() = default;
    MediaFile(const MediaFile &) = delete;
    MediaFile &operator=(const MediaFile &) = delete;

    // Opens and probes the file. On failure returns false and sets *error.
    bool open(const QString &path, QString *error);

    bool isOpen() const { return m_fmt != nullptr; }
    const QString &path() const { return m_path; }
    QString formatName() const;
    // Duration in seconds, or a negative value if unknown.
    double duration() const;
    const std::vector<StreamInfo> &streams() const { return m_streams; }
    // Index of the main video stream, or -1 if the file has no video.
    int bestVideoStream() const { return m_bestVideo; }

    AVFormatContext *formatContext() const { return m_fmt.get(); }

private:
    FormatContextPtr m_fmt;
    QString m_path;
    std::vector<StreamInfo> m_streams;
    int m_bestVideo = -1;
};

} // namespace adscutter
