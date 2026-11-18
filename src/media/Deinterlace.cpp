#include "media/Deinterlace.h"

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace adscutter {

namespace {

// width: samples per line; step: distance between samples of the same
// component (2 for the interleaved chroma of NV12/P010).
template <typename T>
void elaPlane(const uint8_t *src, int srcStride, uint8_t *dst, int dstStride, int width, int height,
              int keptParity, int step)
{
    auto row = [](const uint8_t *base, int stride, int y) {
        return reinterpret_cast<const T *>(base + ptrdiff_t(stride) * y);
    };

    for (int y = 0; y < height; ++y) {
        T *out = reinterpret_cast<T *>(dst + ptrdiff_t(dstStride) * y);
        if ((y & 1) == keptParity) {
            std::memcpy(out, row(src, srcStride, y), size_t(width) * sizeof(T));
            continue;
        }
        // Neighbouring lines of the kept field.
        int yUp = y - 1, yDown = y + 1;
        if (yUp < 0)
            yUp = yDown;
        if (yDown >= height)
            yDown = yUp;
        if (yUp < 0 || yUp >= height) { // single line plane
            std::memcpy(out, row(src, srcStride, y), size_t(width) * sizeof(T));
            continue;
        }
        const T *up = row(src, srcStride, yUp);
        const T *down = row(src, srcStride, yDown);

        for (int x = 0; x < width; ++x) {
            const int vertical = std::abs(int(up[x]) - int(down[x]));
            int best = vertical;
            int value = (int(up[x]) + int(down[x]) + 1) >> 1;
            if (x >= step && x + step < width) {
                // Diagonals: up-left/down-right and up-right/down-left.
                const int d1 = std::abs(int(up[x - step]) - int(down[x + step]));
                const int d2 = std::abs(int(up[x + step]) - int(down[x - step]));
                if (d1 < best) {
                    best = d1;
                    value = (int(up[x - step]) + int(down[x + step]) + 1) >> 1;
                }
                if (d2 < best)
                    value = (int(up[x + step]) + int(down[x - step]) + 1) >> 1;
            }
            out[x] = T(value);
        }
    }
}

} // namespace

FramePtr deinterlaceFrame(const AVFrame *src)
{
    const auto format = AVPixelFormat(src->format);
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(format);
    if (!desc || !(desc->flags & AV_PIX_FMT_FLAG_PLANAR)
        || (desc->flags & (AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_PAL))
        || (desc->flags & AV_PIX_FMT_FLAG_BE))
        return nullptr;
    int bytes = 0;
    for (int c = 0; c < desc->nb_components; ++c) {
        if (desc->comp[c].depth > 16)
            return nullptr;
        const int b = desc->comp[c].depth > 8 ? 2 : 1;
        if (bytes && b != bytes)
            return nullptr;
        bytes = b;
    }

    FramePtr dst(av_frame_alloc());
    if (!dst)
        return nullptr;
    dst->format = src->format;
    dst->width = src->width;
    dst->height = src->height;
    if (av_frame_get_buffer(dst.get(), 0) < 0 || av_frame_copy_props(dst.get(), src) < 0)
        return nullptr;

    // Keep the field shown first: even lines for top field first.
    const int keptParity = (src->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) ? 0 : 1;
    const int planes = av_pix_fmt_count_planes(format);
    for (int p = 0; p < planes; ++p) {
        // Geometry from the components stored in this plane.
        int step = 1;
        bool chroma = false;
        for (int c = 0; c < desc->nb_components; ++c) {
            if (desc->comp[c].plane == p) {
                step = std::max(step, desc->comp[c].step / bytes);
                chroma |= c == 1 || c == 2;
            }
        }
        const int samples = av_image_get_linesize(format, src->width, p) / bytes;
        const int h = chroma ? AV_CEIL_RSHIFT(src->height, desc->log2_chroma_h) : src->height;
        if (bytes == 1)
            elaPlane<uint8_t>(src->data[p], src->linesize[p], dst->data[p], dst->linesize[p], samples, h, keptParity, step);
        else
            elaPlane<uint16_t>(src->data[p], src->linesize[p], dst->data[p], dst->linesize[p], samples, h, keptParity, step);
    }
    dst->flags &= ~AV_FRAME_FLAG_INTERLACED;
    return dst;
}

} // namespace adscutter
