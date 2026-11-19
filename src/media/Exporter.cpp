#include "media/Exporter.h"
#include "media/Bitstream.h"
#include "media/H264Splice.h"
#include "media/Latm.h"
#include "media/MediaFile.h"

#include <QFile>
#include <algorithm>
#include <cstring>

namespace adscutter {

namespace {

// Per-stream export plan.
struct StreamPlan {
    const PacketIndex::Stream *index = nullptr;
    AVStream *in = nullptr;
    AVStream *out = nullptr;
    bool video = false;     // the main video stream
    bool selected = false;  // written to the output
    int nalLength = 0;
    int64_t frameDurationUs = 0;
    std::vector<int32_t> segmentOf; // packet -> segment, -1 = not exported
    uint32_t lastNeeded = 0;        // last packet index (decode order) to export
    bool anyNeeded = false;
    uint32_t next = 0;              // next packet expected from the demuxer
    int64_t lastDts = INT64_MIN;    // in the output time base
    std::vector<uint8_t> paramSets; // last parameter sets seen (video)
    std::unique_ptr<H264Splicer> splicer;
    std::unique_ptr<LatmConverter> latm; // AAC in LATM -> raw AAC
    uint64_t planned = 0, written = 0;
};

// Typical packet duration of a stream, from the spacing of its timestamps.
int64_t typicalDuration(const PacketIndex &index, const PacketIndex::Stream &s, int64_t fallback)
{
    std::vector<int64_t> steps;
    for (size_t i = 1; i < s.displayOrder.size() && steps.size() < 200; ++i) {
        const int64_t d = index.unifiedPts(s, s.displayOrder[i]) - index.unifiedPts(s, s.displayOrder[i - 1]);
        if (d > 0)
            steps.push_back(d);
    }
    if (steps.empty())
        return fallback;
    std::nth_element(steps.begin(), steps.begin() + ptrdiff_t(steps.size() / 2), steps.end());
    return steps[steps.size() / 2];
}

} // namespace

bool exportMatroska(const QString &inputPath, const PacketIndex &index, const ExportSettings &settings,
                    const std::function<bool(double)> &progress, ExportReport *reportOut, QString *error)
{
    ExportReport report;
    auto fail = [&](const QString &why) {
        if (error)
            *error = why;
        return false;
    };

    // --- Validate ---------------------------------------------------------------
    const PacketIndex::Stream *video = index.stream(settings.videoStream);
    if (!video || video->type != AVMEDIA_TYPE_VIDEO || video->displayOrder.empty())
        return fail(QStringLiteral("No video stream to cut"));
    if (settings.segments.empty())
        return fail(QStringLiteral("Nothing to export: no segments"));
    if (settings.streams.empty())
        return fail(QStringLiteral("Nothing to export: no streams selected"));
    const auto frames = uint32_t(video->displayOrder.size());
    for (size_t i = 0; i < settings.segments.size(); ++i) {
        const Segment &seg = settings.segments[i];
        if (seg.in > seg.out || seg.out >= frames || (i > 0 && seg.in <= settings.segments[i - 1].out))
            return fail(QStringLiteral("Invalid segment list"));
        if (!video->isCutPoint(video->packets[video->displayOrder[seg.in]]))
            return fail(QStringLiteral("Segment %1 does not start at a cut point").arg(i + 1));
    }

    QString openError;
    FormatContextPtr in = openFormatContext(inputPath, &openError);
    if (!in)
        return fail(openError);
    if (in->nb_streams != index.streams().size())
        return fail(QStringLiteral("The file does not match its index; rebuild the index"));

    std::vector<StreamPlan> plans(in->nb_streams);
    for (unsigned i = 0; i < in->nb_streams; ++i) {
        StreamPlan &p = plans[i];
        p.index = &index.streams()[i];
        p.in = in->streams[i];
        if (p.in->codecpar->codec_id != p.index->codecId)
            return fail(QStringLiteral("The file does not match its index; rebuild the index"));
        p.video = int(i) == settings.videoStream;
        p.selected = std::find(settings.streams.begin(), settings.streams.end(), int(i)) != settings.streams.end();
        p.nalLength = nalLengthSize(p.index->codecId, p.in->codecpar->extradata, p.in->codecpar->extradata_size);
        p.segmentOf.assign(p.index->packets.size(), -1);
        if (p.video && p.index->codecId == AV_CODEC_ID_H264) {
            p.splicer = std::make_unique<H264Splicer>(p.nalLength);
            p.splicer->addExtradata(p.in->codecpar->extradata, size_t(p.in->codecpar->extradata_size));
        }
    }

    // --- Plan the video of each segment ------------------------------------------
    StreamPlan &vplan = plans[size_t(settings.videoStream)];
    vplan.frameDurationUs = settings.frameDurationUs;
    struct SegmentPlan {
        uint32_t keyPacket, lastPacket;
        int64_t startUs, endUs, offsetUs;
    };
    std::vector<SegmentPlan> segPlans;
    for (size_t si = 0; si < settings.segments.size(); ++si) {
        const Segment &seg = settings.segments[si];
        SegmentPlan sp{};
        sp.keyPacket = video->displayOrder[seg.in];
        sp.lastPacket = sp.keyPacket;
        for (uint32_t pos = seg.in; pos <= seg.out; ++pos)
            sp.lastPacket = std::max(sp.lastPacket, video->displayOrder[pos]);
        sp.startUs = index.unifiedPts(*video, sp.keyPacket);
        sp.endUs = sp.startUs;

        ExportReport::SegmentInfo info;
        info.frames = seg;
        info.sourceStartUs = sp.startUs;
        for (uint32_t d = sp.keyPacket; d <= sp.lastPacket; ++d) {
            const int32_t pos = video->displayPosOf[d];
            if (pos < int32_t(seg.in))
                continue; // open-GOP leading picture (or not displayable)
            vplan.segmentOf[d] = int32_t(si);
            ++vplan.planned;
            if (pos > int32_t(seg.out)) {
                ++info.extraFrames;
                info.extraPositions.push_back(uint32_t(pos));
            }
            sp.endUs = std::max(sp.endUs, index.unifiedPts(*video, d) + settings.frameDurationUs);
        }
        std::sort(info.extraPositions.begin(), info.extraPositions.end());
        info.durationUs = sp.endUs - sp.startUs;
        report.segments.push_back(info);
        segPlans.push_back(sp);
    }

    // Output offsets: segments are laid end to end, without gaps.
    //
    // Decode timestamps at a cut can come out lower than the previous
    // segment's last ones (the cut point's decode time is several frames
    // before its presentation, a slack normally filled by the leading
    // pictures that were dropped). Matroska does not store decode times;
    // the muxer only needs them increasing and not after the presentation
    // time, which the per-packet clamp below guarantees.
    int64_t cursor = 0;
    for (SegmentPlan &sp : segPlans) {
        sp.offsetUs = cursor - sp.startUs;
        cursor += sp.endUs - sp.startUs;
    }
    report.durationUs = cursor;

    // Video decode timestamps are synthesized rather than copied: the source
    // may have none (Matroska stores presentation times only), and dropped
    // leading pictures leave holes at every cut. The k-th packet in decode
    // order gets the k-th smallest presentation time, minus the largest
    // reordering delay: always increasing and never after its presentation.
    std::vector<int64_t> videoDtsUs(video->packets.size(), INT64_MIN);
    {
        std::vector<std::pair<uint32_t, int64_t>> order; // packet, output pts
        for (size_t si = 0; si < segPlans.size(); ++si) {
            for (uint32_t d = segPlans[si].keyPacket; d <= segPlans[si].lastPacket; ++d) {
                if (vplan.segmentOf[d] == int32_t(si))
                    order.emplace_back(d, index.unifiedPts(*video, d) + segPlans[si].offsetUs);
            }
        }
        std::vector<int64_t> sorted;
        sorted.reserve(order.size());
        for (const auto &o : order)
            sorted.push_back(o.second);
        std::sort(sorted.begin(), sorted.end());
        int64_t delay = 0;
        for (size_t k = 0; k < order.size(); ++k)
            delay = std::max(delay, sorted[k] - order[k].second);
        for (size_t k = 0; k < order.size(); ++k)
            videoDtsUs[order[k].first] = sorted[k] - delay;
    }

    // --- Plan the other streams ----------------------------------------------------
    for (StreamPlan &p : plans) {
        if (p.video || !p.selected)
            continue;
        const bool audio = p.index->type == AVMEDIA_TYPE_AUDIO;
        p.frameDurationUs = audio ? typicalDuration(index, *p.index, 24000) : 0;
        const auto &order = p.index->displayOrder;
        // Audio: frames whose middle falls inside the segment, using each
        // frame's own duration (up to the next one): Vorbis, for one,
        // switches between short and long frames. Others (subtitles): those
        // starting inside it.
        auto anchor = [&](uint32_t pos) {
            const int64_t start = index.unifiedPts(*p.index, order[pos]);
            if (!audio)
                return start;
            const int64_t next = pos + 1 < order.size() ? index.unifiedPts(*p.index, order[pos + 1])
                                                         : start + p.frameDurationUs;
            const int64_t duration = next > start && next - start <= 4 * p.frameDurationUs ? next - start
                                                                                           : p.frameDurationUs;
            return start + duration / 2;
        };
        for (size_t si = 0; si < segPlans.size(); ++si) {
            auto firstFrom = [&](int64_t t) {
                return uint32_t(std::partition_point(order.begin(), order.end(),
                                                     [&](const uint32_t &packet) {
                                                         return anchor(uint32_t(&packet - order.data())) < t;
                                                     })
                                - order.begin());
            };
            const uint32_t from = firstFrom(segPlans[si].startUs);
            const uint32_t to = firstFrom(segPlans[si].endUs);
            for (uint32_t pos = from; pos < to; ++pos) {
                p.segmentOf[order[pos]] = int32_t(si);
                ++p.planned;
            }
        }
    }
    for (StreamPlan &p : plans) {
        if (!p.selected)
            continue;
        for (uint32_t i = 0; i < p.segmentOf.size(); ++i) {
            if (p.segmentOf[i] >= 0) {
                p.lastNeeded = i;
                p.anyNeeded = true;
            }
        }
    }

    // --- AAC in LATM: find its configuration before writing the header ----------
    bool anyLatm = false;
    for (StreamPlan &p : plans) {
        if (p.selected && p.index->codecId == AV_CODEC_ID_AAC_LATM) {
            p.latm = std::make_unique<LatmConverter>();
            anyLatm = true;
        }
    }
    if (anyLatm) {
        FormatContextPtr scan = openFormatContext(inputPath, &openError);
        if (!scan)
            return fail(openError);
        PacketPtr sp(av_packet_alloc());
        std::vector<std::vector<uint8_t>> ignored;
        for (int n = 0; n < 200000; ++n) {
            const bool missing = std::any_of(plans.begin(), plans.end(),
                                             [](const StreamPlan &p) { return p.latm && !p.latm->hasConfig(); });
            if (!missing || av_read_frame(scan.get(), sp.get()) < 0)
                break;
            if (sp->stream_index >= 0 && unsigned(sp->stream_index) < plans.size()) {
                StreamPlan &p = plans[size_t(sp->stream_index)];
                std::string why;
                ignored.clear();
                if (p.latm && !p.latm->hasConfig() && !p.latm->convert(sp->data, size_t(sp->size), ignored, &why)) {
                    av_packet_unref(sp.get());
                    return fail(QStringLiteral("Audio stream %1: %2").arg(sp->stream_index).arg(QString::fromStdString(why)));
                }
            }
            av_packet_unref(sp.get());
        }
        for (const StreamPlan &p : plans) {
            if (p.latm && !p.latm->hasConfig())
                return fail(QStringLiteral("Audio stream %1: no AAC configuration found in the LATM stream").arg(p.in->index));
        }
    }

    // --- Output file ----------------------------------------------------------------
    const QString partPath = settings.outputPath + QStringLiteral(".part");
    QFile::remove(partPath);
    AVFormatContext *rawOut = nullptr;
    const QByteArray partPathUtf8 = partPath.toUtf8();
    int ret = avformat_alloc_output_context2(&rawOut, nullptr, "matroska", partPathUtf8.constData());
    if (ret < 0 || !rawOut)
        return fail(QStringLiteral("Cannot create output: %1").arg(avErrorString(ret)));
    struct OutputCloser {
        AVFormatContext *ctx;
        bool opened = false;
        ~OutputCloser()
        {
            if (opened)
                avio_closep(&ctx->pb);
            avformat_free_context(ctx);
        }
    } out{rawOut};

    for (int si : settings.streams) {
        if (si < 0 || unsigned(si) >= plans.size())
            return fail(QStringLiteral("Invalid stream %1").arg(si));
        StreamPlan &p = plans[size_t(si)];
        AVStream *ost = avformat_new_stream(out.ctx, nullptr);
        if (!ost || avcodec_parameters_copy(ost->codecpar, p.in->codecpar) < 0)
            return fail(QStringLiteral("Out of memory"));
        ost->codecpar->codec_tag = 0;
        if (p.latm) {
            // Stored as plain AAC: the configuration becomes the codec private data.
            const std::vector<uint8_t> &asc = p.latm->audioSpecificConfig();
            ost->codecpar->codec_id = AV_CODEC_ID_AAC;
            av_freep(&ost->codecpar->extradata);
            ost->codecpar->extradata = static_cast<uint8_t *>(av_mallocz(asc.size() + AV_INPUT_BUFFER_PADDING_SIZE));
            if (!ost->codecpar->extradata)
                return fail(QStringLiteral("Out of memory"));
            std::memcpy(ost->codecpar->extradata, asc.data(), asc.size());
            ost->codecpar->extradata_size = int(asc.size());
        }
        ost->time_base = p.in->time_base;
        ost->disposition = p.in->disposition;
        av_dict_copy(&ost->metadata, p.in->metadata, 0);
        if (p.index->type == AVMEDIA_TYPE_VIDEO) {
            ost->sample_aspect_ratio = av_guess_sample_aspect_ratio(in.get(), p.in, nullptr);
            ost->codecpar->sample_aspect_ratio = ost->sample_aspect_ratio;
            ost->avg_frame_rate = p.in->avg_frame_rate;
            ost->r_frame_rate = p.in->r_frame_rate;
        }
        p.out = ost;
    }

    ret = avio_open(&out.ctx->pb, partPathUtf8.constData(), AVIO_FLAG_WRITE);
    if (ret < 0)
        return fail(QStringLiteral("Cannot write %1: %2").arg(partPath, avErrorString(ret)));
    out.opened = true;
    out.ctx->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_ZERO;
    ret = avformat_write_header(out.ctx, nullptr);
    if (ret < 0) {
        QFile::remove(partPath);
        return fail(QStringLiteral("Cannot write header: %1").arg(avErrorString(ret)));
    }

    // --- Copy packets -------------------------------------------------------------------
    auto abort = [&](const QString &why) {
        avio_closep(&out.ctx->pb);
        out.opened = false;
        QFile::remove(partPath);
        return fail(why);
    };

    PacketPtr pkt(av_packet_alloc());
    bool spliceWarned = false, latmWarned = false;
    const int64_t fileSize = avio_size(in->pb);
    uint64_t count = 0;
    for (;;) {
        // Stop as soon as every selected stream is complete.
        bool done = true;
        for (const StreamPlan &p : plans) {
            if (p.selected && p.anyNeeded && p.next <= p.lastNeeded)
                done = false;
        }
        if (done)
            break;

        ret = av_read_frame(in.get(), pkt.get());
        if (ret == AVERROR(EAGAIN))
            continue;
        if (ret < 0)
            break;
        if (++count % 256 == 0 && progress && fileSize > 0
            && !progress(std::min(1.0, double(avio_tell(in->pb)) / double(fileSize)))) {
            av_packet_unref(pkt.get());
            return abort({});
        }

        if (pkt->stream_index < 0 || unsigned(pkt->stream_index) >= plans.size()) {
            av_packet_unref(pkt.get());
            continue;
        }
        StreamPlan &p = plans[size_t(pkt->stream_index)];
        if (!p.selected && !p.video) {
            av_packet_unref(pkt.get());
            continue;
        }
        const uint32_t i = p.next++;
        if (i >= p.index->packets.size() || p.index->packets[i].size != pkt->size
            || (p.index->packets[i].pos >= 0 && p.index->packets[i].pos != pkt->pos)) {
            av_packet_unref(pkt.get());
            return abort(QStringLiteral("The file does not match its index; rebuild the index"));
        }
        const PacketIndex::Packet &entry = p.index->packets[i];

        const int32_t si = p.segmentOf[i];
        const bool isSegmentStart = p.video && si >= 0 && segPlans[size_t(si)].keyPacket == i;
        if (p.video && (entry.flags & PacketIndex::ParamSets)) {
            auto params = extractParamSets(p.index->codecId, pkt->data, size_t(pkt->size), p.nalLength);
            if (!params.empty()) {
                if (p.splicer)
                    p.splicer->addParameterSets(params.data(), params.size());
                p.paramSets = std::move(params);
            }
        }
        if (!p.selected || si < 0) {
            av_packet_unref(pkt.get());
            continue;
        }

        if (isSegmentStart) {
            // Make the packet self-contained: parameter sets in front, and
            // for HEVC open GOPs a BLA instead of a CRA.
            const bool needParams = !(entry.flags & PacketIndex::ParamSets) && !p.paramSets.empty();
            if (needParams || p.index->codecId == AV_CODEC_ID_HEVC) {
                PacketPtr copy(av_packet_alloc());
                const size_t prefix = needParams ? p.paramSets.size() : 0;
                if (av_new_packet(copy.get(), int(prefix + size_t(pkt->size))) < 0) {
                    av_packet_unref(pkt.get());
                    return abort(QStringLiteral("Out of memory"));
                }
                av_packet_copy_props(copy.get(), pkt.get());
                if (prefix)
                    std::copy(p.paramSets.begin(), p.paramSets.end(), copy->data);
                std::copy(pkt->data, pkt->data + pkt->size, copy->data + prefix);
                if (needParams)
                    ++report.paramSetsInserted;
                if (p.index->codecId == AV_CODEC_ID_HEVC
                    && hevcCraToBla(copy->data + prefix, size_t(pkt->size), p.nalLength))
                    ++report.blaConversions;
                av_packet_unref(pkt.get());
                av_packet_move_ref(pkt.get(), copy.get());
            }
        }

        // H.264: make the segment start a real IDR (see H264Splice.h).
        if (p.splicer && si >= 0) {
            std::vector<uint8_t> rewritten;
            std::string why;
            bool changed;
            if (isSegmentStart) {
                changed = p.splicer->startSegment(pkt->data, size_t(pkt->size), int(si & 1), rewritten, &why);
                if (changed)
                    ++report.idrConversions;
                else if (!why.empty() && !spliceWarned) {
                    report.warnings << QStringLiteral("Cannot convert an H.264 cut point to IDR (%1); "
                                                      "players may show glitches right after cuts")
                                           .arg(QString::fromStdString(why));
                    spliceWarned = true;
                }
            } else {
                changed = p.splicer->continueSegment(pkt->data, size_t(pkt->size), rewritten);
            }
            if (changed) {
                PacketPtr copy(av_packet_alloc());
                if (av_new_packet(copy.get(), int(rewritten.size())) < 0) {
                    av_packet_unref(pkt.get());
                    return abort(QStringLiteral("Out of memory"));
                }
                av_packet_copy_props(copy.get(), pkt.get());
                std::copy(rewritten.begin(), rewritten.end(), copy->data);
                av_packet_unref(pkt.get());
                av_packet_move_ref(pkt.get(), copy.get());
            }
        }

        // Timestamps: unified time shifted by the segment's offset.
        const int64_t offset = segPlans[size_t(si)].offsetUs;
        const int64_t ptsUs = index.unifiedPts(*p.index, i) + offset;
        const int64_t dtsUs = p.video ? videoDtsUs[i]
                              : p.index->type == AVMEDIA_TYPE_VIDEO ? index.unifiedDts(*p.index, i) + offset
                                                                    : ptsUs;
        int64_t pts = av_rescale_q(ptsUs, AV_TIME_BASE_Q, p.out->time_base);
        int64_t dts = av_rescale_q(dtsUs, AV_TIME_BASE_Q, p.out->time_base);
        if (p.lastDts != INT64_MIN && dts <= p.lastDts)
            dts = p.lastDts + 1;
        if (pts < dts)
            pts = dts;
        p.lastDts = dts;
        pkt->pts = pts;
        pkt->dts = dts;
        pkt->duration = av_rescale_q(pkt->duration, p.in->time_base, p.out->time_base);
        if (p.video || p.index->type == AVMEDIA_TYPE_VIDEO) {
            if (p.index->isCutPoint(entry) || isSegmentStart)
                pkt->flags |= AV_PKT_FLAG_KEY;
            else
                pkt->flags &= ~AV_PKT_FLAG_KEY;
        }
        pkt->stream_index = p.out->index;
        pkt->pos = -1;

        if (p.latm) {
            // One LOAS frame may carry several AAC frames: one packet each.
            std::vector<std::vector<uint8_t>> frames;
            std::string why;
            if (!p.latm->convert(pkt->data, size_t(pkt->size), frames, &why)) {
                av_packet_unref(pkt.get());
                return abort(QStringLiteral("Audio stream %1: %2").arg(p.in->index).arg(QString::fromStdString(why)));
            }
            if (p.latm->configChanged() && !latmWarned) {
                report.warnings << QStringLiteral("The AAC audio format changes during the recording; "
                                                  "only the first format is declared in the output");
                latmWarned = true;
            }
            const int64_t total = pkt->duration > 0 ? pkt->duration
                                                    : av_rescale_q(p.frameDurationUs, AV_TIME_BASE_Q, p.out->time_base);
            const int64_t base = pkt->pts;
            for (size_t k = 0; k < frames.size(); ++k) {
                PacketPtr frame(av_packet_alloc());
                if (av_new_packet(frame.get(), int(frames[k].size())) < 0) {
                    av_packet_unref(pkt.get());
                    return abort(QStringLiteral("Out of memory"));
                }
                std::memcpy(frame->data, frames[k].data(), frames[k].size());
                int64_t ts = base + total * int64_t(k) / int64_t(frames.size());
                if (k > 0 && ts <= p.lastDts)
                    ts = p.lastDts + 1;
                frame->pts = frame->dts = ts;
                p.lastDts = ts;
                frame->duration = total / int64_t(frames.size());
                frame->flags = AV_PKT_FLAG_KEY;
                frame->stream_index = p.out->index;
                ret = av_interleaved_write_frame(out.ctx, frame.get());
                if (ret < 0) {
                    av_packet_unref(pkt.get());
                    return abort(QStringLiteral("Write error: %1").arg(avErrorString(ret)));
                }
                ++report.packetsWritten;
            }
            av_packet_unref(pkt.get());
            ++p.written;
            continue;
        }

        ret = av_interleaved_write_frame(out.ctx, pkt.get());
        if (ret < 0)
            return abort(QStringLiteral("Write error: %1").arg(avErrorString(ret)));
        ++p.written;
        ++report.packetsWritten;
    }

    ret = av_write_trailer(out.ctx);
    if (ret < 0)
        return abort(QStringLiteral("Cannot finish the file: %1").arg(avErrorString(ret)));
    avio_closep(&out.ctx->pb);
    out.opened = false;

    for (const StreamPlan &p : plans) {
        if (p.selected && p.written != p.planned) {
            QFile::remove(partPath);
            return fail(QStringLiteral("Stream %1: %2 of %3 packets found; the recording may be damaged")
                            .arg(p.in->index)
                            .arg(p.written)
                            .arg(p.planned));
        }
    }

    QFile::remove(settings.outputPath);
    if (!QFile::rename(partPath, settings.outputPath)) {
        QFile::remove(partPath);
        return fail(QStringLiteral("Cannot rename %1 to %2").arg(partPath, settings.outputPath));
    }

    uint32_t extra = 0;
    for (const auto &s : report.segments)
        extra += s.extraFrames;
    if (extra > 0)
        report.warnings << QStringLiteral(
                               "%1 segment end(s) include up to %2 extra frame(s) that later frames depend on "
                               "(choose an I or P frame as the end to avoid this)")
                               .arg(std::count_if(report.segments.begin(), report.segments.end(),
                                                  [](const auto &s) { return s.extraFrames > 0; }))
                               .arg(std::max_element(report.segments.begin(), report.segments.end(),
                                                     [](const auto &a, const auto &b) { return a.extraFrames < b.extraFrames; })
                                        ->extraFrames);
    if (progress)
        progress(1.0);
    if (reportOut)
        *reportOut = std::move(report);
    return true;
}

} // namespace adscutter
