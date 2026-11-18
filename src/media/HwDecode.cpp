#include "media/HwDecode.h"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#include <map>
#include <vector>
#include <mutex>

namespace adscutter {

namespace {

// One device per type for the whole process: creating a Vulkan device takes
// a while, and decoders come and go with every seek-heavy session.
struct DeviceCache {
    std::mutex mutex;
    std::map<AVHWDeviceType, AVBufferRef *> devices; // null = creation failed
    ~DeviceCache()
    {
        for (auto &[type, ref] : devices)
            av_buffer_unref(&ref);
    }
};

DeviceCache &deviceCache()
{
    static DeviceCache cache;
    return cache;
}

AVBufferRef *device(AVHWDeviceType type)
{
    DeviceCache &cache = deviceCache();
    std::lock_guard lock(cache.mutex);
    auto it = cache.devices.find(type);
    if (it == cache.devices.end()) {
        AVBufferRef *ref = nullptr;
        if (av_hwdevice_ctx_create(&ref, type, nullptr, nullptr, 0) < 0)
            ref = nullptr;
        it = cache.devices.emplace(type, ref).first;
    }
    return it->second ? av_buffer_ref(it->second) : nullptr;
}

AVPixelFormat getFormat(AVCodecContext *ctx, const AVPixelFormat *formats)
{
    auto *state = static_cast<HwDecodeState *>(ctx->opaque);
    for (const AVPixelFormat *f = formats; *f != AV_PIX_FMT_NONE; ++f) {
        if (state && *f == state->pixFmt) {
            state->active = true;
            return *f;
        }
    }
    // Hardware refused (or not offered for this stream): first software format.
    if (state)
        state->active = false;
    for (const AVPixelFormat *f = formats; *f != AV_PIX_FMT_NONE; ++f) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(*f);
        if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL))
            return *f;
    }
    return AV_PIX_FMT_NONE;
}

} // namespace

HwPreference hwPreferenceFromString(const QString &name)
{
    const QString n = name.trimmed().toLower();
    if (n == QStringLiteral("vulkan"))
        return HwPreference::Vulkan;
    if (n == QStringLiteral("vaapi"))
        return HwPreference::Vaapi;
    if (n == QStringLiteral("none") || n == QStringLiteral("off") || n == QStringLiteral("software"))
        return HwPreference::Off;
    return HwPreference::Auto;
}

QString hwPreferenceName(HwPreference preference)
{
    switch (preference) {
    case HwPreference::Vulkan: return QStringLiteral("vulkan");
    case HwPreference::Vaapi: return QStringLiteral("vaapi");
    case HwPreference::Off: return QStringLiteral("none");
    case HwPreference::Auto: break;
    }
    return QStringLiteral("auto");
}

HwPreference effectiveHwPreference(HwPreference fallback)
{
    const QByteArray env = qgetenv("ADSCUTTER_HWDEC");
    return env.isEmpty() ? fallback : hwPreferenceFromString(QString::fromLatin1(env));
}

QString HwDecodeState::name() const
{
    if (!active)
        return QStringLiteral("software");
    switch (type) {
    case AV_HWDEVICE_TYPE_VULKAN: return QStringLiteral("Vulkan");
    case AV_HWDEVICE_TYPE_VAAPI: return QStringLiteral("VAAPI");
    default: return QString::fromUtf8(av_hwdevice_get_type_name(type));
    }
}

bool setupHwDecoding(AVCodecContext *ctx, const AVCodec *codec, HwPreference preference, HwDecodeState *state)
{
    *state = {};
    std::vector<AVHWDeviceType> order;
    switch (preference) {
    case HwPreference::Auto: order = {AV_HWDEVICE_TYPE_VULKAN, AV_HWDEVICE_TYPE_VAAPI}; break;
    case HwPreference::Vulkan: order = {AV_HWDEVICE_TYPE_VULKAN}; break;
    case HwPreference::Vaapi: order = {AV_HWDEVICE_TYPE_VAAPI}; break;
    case HwPreference::Off: return false;
    }

    for (AVHWDeviceType type : order) {
        // Does this decoder have a hardware path of that type?
        AVPixelFormat pixFmt = AV_PIX_FMT_NONE;
        for (int i = 0;; ++i) {
            const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);
            if (!config)
                break;
            if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && config->device_type == type) {
                pixFmt = config->pix_fmt;
                break;
            }
        }
        if (pixFmt == AV_PIX_FMT_NONE)
            continue;
        // VAAPI returned wrong pictures (an exact copy of a neighbouring
        // frame) after seeks in interlaced H.264 on Mesa/radeonsi, while
        // Vulkan on the same GPU was bit exact: don't use it there.
        const bool interlaced = ctx->field_order != AV_FIELD_PROGRESSIVE && ctx->field_order != AV_FIELD_UNKNOWN;
        if (type == AV_HWDEVICE_TYPE_VAAPI && codec->id == AV_CODEC_ID_H264 && interlaced)
            continue;
        AVBufferRef *ref = device(type);
        if (!ref)
            continue;
        ctx->hw_device_ctx = ref;
        state->type = type;
        state->pixFmt = pixFmt;
        ctx->opaque = state;
        ctx->get_format = getFormat;
        // A few surfaces more than the codec needs: frames are copied to
        // memory right away, but the copy happens after the decoder moved on.
        ctx->extra_hw_frames = 4;
        return true;
    }
    return false;
}

FramePtr downloadHwFrame(const AVFrame *frame)
{
    FramePtr sw(av_frame_alloc());
    if (!sw || av_hwframe_transfer_data(sw.get(), frame, 0) < 0 || av_frame_copy_props(sw.get(), frame) < 0)
        return nullptr;
    return sw;
}

} // namespace adscutter
