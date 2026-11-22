// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ANDROID_V4L2_CODEC2_COMMON_GPU_CONVERTER_H
#define ANDROID_V4L2_CODEC2_COMMON_GPU_CONVERTER_H

#include <stdint.h>

#include <memory>

namespace android {

// Converts RGBA frames to NV12 on the GPU, from one dma-buf into another. For devices whose
// encoder only takes YUV, where reading the GPU's write-combined RGBA buffers back with the CPU is
// far too slow for video.
//
// An instance keeps an EGL context current on the thread that created it, and must only be used
// on that thread.
class GpuConverter {
public:
    struct Plane {
        int fd;
        uint32_t offset;
        uint32_t stride;  // in bytes
    };

    static std::unique_ptr<GpuConverter> create();
    ~GpuConverter();

    // Converts the |width|x|height| RGBA (R, G, B, A bytes) image in |src| into the NV12 planes
    // |dstY| and |dstUV|. Returns once the GPU is done, or, given |fenceFd|, as soon as the work is
    // submitted, with |*fenceFd| a sync file that signals when the GPU is done, or -1 if it is
    // already.
    bool convertRgbaToNv12(const Plane& src, uint32_t srcWidth, uint32_t srcHeight,
                           const Plane& dstY, const Plane& dstUV, uint32_t width,
                           uint32_t height, int* fenceFd = nullptr);

private:
    GpuConverter() = default;
    bool initialize();
    // EGL and GL objects, kept opaque here so users need no EGL headers.
    void* importImage(const Plane& plane, uint32_t fourcc, uint32_t width, uint32_t height);
    bool drawInto(void* image, uint32_t program, uint32_t width, uint32_t height);

    void* mDisplay = nullptr;
    void* mContext = nullptr;
    void* mSurface = nullptr;
    uint32_t mProgramY = 0;
    uint32_t mProgramUV = 0;
    uint32_t mSrcTexture = 0;
    uint32_t mDstTexture = 0;
    uint32_t mFramebuffer = 0;
};

}  // namespace android

#endif  // ANDROID_V4L2_CODEC2_COMMON_GPU_CONVERTER_H
