#include "media/MediaFile.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
}

namespace adscutter {

namespace {

QString metadataValue(const AVDictionary *dict, const char *key)
{
    const AVDictionaryEntry *e = av_dict_get(dict, key, nullptr, 0);
    return e ? QString::fromUtf8(e->value) : QString();
}

QString fieldOrderName(AVFieldOrder order)
{
    switch (order) {
    case AV_FIELD_PROGRESSIVE: return QStringLiteral("progressive");
    // The second letter is the field displayed first.
    case AV_FIELD_TT:
    case AV_FIELD_BT: return QStringLiteral("interlaced TFF");
    case AV_FIELD_BB:
    case AV_FIELD_TB: return QStringLiteral("interlaced BFF");
    default: return {};
    }
}

void checkMkvCompatibility(StreamInfo &info)
{
    static const AVOutputFormat *mkv = av_guess_format("matroska", nullptr, nullptr);

    if (info.codecId == AV_CODEC_ID_NONE) {
        info.exportNote = QStringLiteral("Unknown codec");
        return;
    }
    if (info.codecId == AV_CODEC_ID_AAC_LATM) {
        // Not accepted by the Matroska muxer as-is; a LATM -> raw AAC
        // repacketizer is planned (milestone 6).
        info.exportNote = QStringLiteral("AAC LATM needs repacketizing (not yet supported)");
        return;
    }
    if (info.codecId == AV_CODEC_ID_DVB_TELETEXT) {
        info.exportNote = QStringLiteral("Teletext cannot be stored in Matroska");
        return;
    }
    if (mkv && avformat_query_codec(mkv, info.codecId, FF_COMPLIANCE_NORMAL) == 1) {
        info.mkvCompatible = true;
    } else {
        info.exportNote = QStringLiteral("Codec not supported by Matroska");
    }
}

} // namespace

QString StreamInfo::typeName() const
{
    switch (type) {
    case AVMEDIA_TYPE_VIDEO: return QStringLiteral("Video");
    case AVMEDIA_TYPE_AUDIO: return QStringLiteral("Audio");
    case AVMEDIA_TYPE_SUBTITLE: return QStringLiteral("Subtitle");
    case AVMEDIA_TYPE_DATA: return QStringLiteral("Data");
    case AVMEDIA_TYPE_ATTACHMENT: return QStringLiteral("Attachment");
    default: return QStringLiteral("Unknown");
    }
}

QString StreamInfo::details() const
{
    QStringList parts;
    if (type == AVMEDIA_TYPE_VIDEO) {
        if (width > 0 && height > 0)
            parts << QStringLiteral("%1x%2").arg(width).arg(height);
        if (sampleAspect.num > 0 && sampleAspect.den > 0
            && sampleAspect.num != sampleAspect.den) {
            AVRational dar;
            av_reduce(&dar.num, &dar.den, int64_t(width) * sampleAspect.num,
                      int64_t(height) * sampleAspect.den, 1024 * 1024);
            parts << QStringLiteral("DAR %1:%2").arg(dar.num).arg(dar.den);
        }
        if (frameRate.num > 0 && frameRate.den > 0)
            parts << QStringLiteral("%1 fps").arg(av_q2d(frameRate), 0, 'f', 3);
        const QString fo = fieldOrderName(fieldOrder);
        if (!fo.isEmpty())
            parts << fo;
    } else if (type == AVMEDIA_TYPE_AUDIO) {
        if (sampleRate > 0)
            parts << QStringLiteral("%1 Hz").arg(sampleRate);
        if (!channelLayout.isEmpty())
            parts << channelLayout;
        else if (channels > 0)
            parts << QStringLiteral("%1 ch").arg(channels);
    }
    if (bitRate > 0)
        parts << QStringLiteral("%1 kb/s").arg(bitRate / 1000);
    if (programId >= 0)
        parts << QStringLiteral("program %1").arg(programId);
    if (hearingImpaired)
        parts << QStringLiteral("hearing impaired");
    if (visualImpaired)
        parts << QStringLiteral("audio description");
    return parts.join(QStringLiteral(", "));
}

bool MediaFile::open(const QString &path, QString *error)
{
    m_fmt.reset();
    m_streams.clear();
    m_bestVideo = -1;
    m_path.clear();

    AVDictionary *opts = nullptr;
    // TV recordings may announce streams (e.g. subtitles) late: probe more
    // than the default so the stream list is complete.
    av_dict_set(&opts, "probesize", "50000000", 0);
    av_dict_set(&opts, "analyzeduration", "10000000", 0);

    AVFormatContext *raw = nullptr;
    const QByteArray encoded = path.toUtf8();
    int ret = avformat_open_input(&raw, encoded.constData(), nullptr, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        if (error)
            *error = QStringLiteral("Cannot open file: %1").arg(avErrorString(ret));
        return false;
    }
    FormatContextPtr fmt(raw);

    ret = avformat_find_stream_info(fmt.get(), nullptr);
    if (ret < 0) {
        if (error)
            *error = QStringLiteral("Cannot read stream information: %1").arg(avErrorString(ret));
        return false;
    }

    std::vector<int> programOf(fmt->nb_streams, -1);
    if (fmt->nb_programs > 1) {
        for (unsigned p = 0; p < fmt->nb_programs; ++p) {
            const AVProgram *prog = fmt->programs[p];
            for (unsigned i = 0; i < prog->nb_stream_indexes; ++i)
                programOf[prog->stream_index[i]] = prog->program_num;
        }
    }

    m_streams.reserve(fmt->nb_streams);
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVStream *st = fmt->streams[i];
        const AVCodecParameters *par = st->codecpar;

        StreamInfo info;
        info.index = int(i);
        info.type = par->codec_type;
        info.codecId = par->codec_id;
        info.codecName = QString::fromUtf8(avcodec_get_name(par->codec_id));
        if (const AVCodecDescriptor *desc = avcodec_descriptor_get(par->codec_id); desc && desc->long_name)
            info.codecName = QString::fromUtf8(desc->long_name);
        info.language = metadataValue(st->metadata, "language");
        info.title = metadataValue(st->metadata, "title");
        info.programId = programOf[i];
        info.bitRate = par->bit_rate;
        info.isDefault = st->disposition & AV_DISPOSITION_DEFAULT;
        info.hearingImpaired = st->disposition & AV_DISPOSITION_HEARING_IMPAIRED;
        info.visualImpaired = st->disposition & AV_DISPOSITION_VISUAL_IMPAIRED;

        if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
            info.width = par->width;
            info.height = par->height;
            info.sampleAspect = av_guess_sample_aspect_ratio(fmt.get(), const_cast<AVStream *>(st), nullptr);
            info.frameRate = av_guess_frame_rate(fmt.get(), const_cast<AVStream *>(st), nullptr);
            info.fieldOrder = par->field_order;
        } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
            info.sampleRate = par->sample_rate;
            info.channels = par->ch_layout.nb_channels;
            char layout[128] = {};
            if (par->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC
                && av_channel_layout_describe(&par->ch_layout, layout, sizeof(layout)) > 0)
                info.channelLayout = QString::fromUtf8(layout);
        }

        checkMkvCompatibility(info);
        m_streams.push_back(std::move(info));
    }

    const int best = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    m_bestVideo = best >= 0 ? best : -1;

    m_fmt = std::move(fmt);
    m_path = path;
    return true;
}

QString MediaFile::formatName() const
{
    if (!m_fmt || !m_fmt->iformat)
        return {};
    return QString::fromUtf8(m_fmt->iformat->long_name ? m_fmt->iformat->long_name
                                                       : m_fmt->iformat->name);
}

double MediaFile::duration() const
{
    if (!m_fmt || m_fmt->duration == AV_NOPTS_VALUE)
        return -1.0;
    return double(m_fmt->duration) / AV_TIME_BASE;
}

} // namespace adscutter
