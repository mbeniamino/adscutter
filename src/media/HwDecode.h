#pragma once

#include "media/FFmpegUtil.h"

#include <QString>

namespace adscutter {

// Hardware video decoding through FFmpeg's device API.
enum class HwPreference {
    Auto,   // Vulkan, then VAAPI, then software
    Vulkan,
    Vaapi,
    Off,
};

HwPreference hwPreferenceFromString(const QString &name); // "auto", "vulkan", "vaapi", "none"/"off"
QString hwPreferenceName(HwPreference preference);
// The preference set by the ADSCUTTER_HWDEC environment variable, if any,
// otherwise `fallback`.
HwPreference effectiveHwPreference(HwPreference fallback);

// Per decoder state; must outlive the codec context it is attached to.
struct HwDecodeState {
    AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
    AVPixelFormat pixFmt = AV_PIX_FMT_NONE;
    bool active = false; // the decoder actually produces hardware frames

    QString name() const; // "Vulkan", "VAAPI" or "software"
};

// Configures a codec context (before avcodec_open2) for hardware decoding,
// trying the backends allowed by `preference`. Returns false (context
// untouched) when none applies; decoding is then done in software. Even
// when it returns true the decoder may fall back to software if the device
// refuses the stream: check state->active after the first frame.
bool setupHwDecoding(AVCodecContext *ctx, const AVCodec *codec, HwPreference preference, HwDecodeState *state);

// Copies a hardware frame to system memory (with its properties). Returns
// nullptr on failure. Software frames are not accepted.
FramePtr downloadHwFrame(const AVFrame *frame);

} // namespace adscutter
