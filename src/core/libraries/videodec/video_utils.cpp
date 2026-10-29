// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_utils.h"

#include "common/alignment.h"
#include "common/logging/log.h"

#include <libavutil/frame.h>

#include <cstring>

namespace Libraries::Videodec {

bool CopyNV12Data(u8* dst, u64 max_size, const AVFrame& src) {
    // A decoder that failed to produce a picture, or a stream that switched to a pixel format
    // without chroma, leaves these null. Dereferencing them here crashed on AVC streams that
    // contain a resolution change or a corrupt access unit.
    if (!dst || !src.data[0] || !src.data[1] || src.width <= 0 || src.height < 2) {
        LOG_ERROR(Lib_Videodec, "CopyNV12Data: invalid frame dst={}, w={}, h={}, y={}, uv={}",
                  static_cast<void*>(dst), src.width, src.height, static_cast<void*>(src.data[0]),
                  static_cast<void*>(src.data[1]));
        return false;
    }

    const auto dst_pitch = Common::AlignUp<u32>(src.width, 64);
    const auto dst_height = Common::AlignUp<u32>(src.height, 16);
    const u64 required = (static_cast<u64>(dst_pitch) * dst_height * 3) / 2;

    // max_size is the real capacity of the guest frame buffer. This used to be an ASSERT against a
    // size derived from the frame itself, so in release builds the check could never fail and a
    // stream larger than the decoder was configured for silently overwrote guest memory.
    if (required > max_size) {
        LOG_ERROR(Lib_Videodec,
                  "CopyNV12Data: frame {}x{} needs {:#x} bytes but the frame buffer only has {:#x}",
                  src.width, src.height, required, max_size);
        return false;
    }

    const auto luma_dst = dst;
    const auto chroma_dst = dst + dst_pitch * dst_height;

    if (src.width != dst_pitch) {
        for (u32 y = 0; y < src.height; ++y) {
            std::memcpy(luma_dst + y * dst_pitch, src.data[0] + y * src.linesize[0], src.width);
        }
        for (u32 y = 0; y < src.height / 2; ++y) {
            std::memcpy(chroma_dst + y * dst_pitch, src.data[1] + y * src.linesize[1], src.width);
        }
    } else {
        std::memcpy(luma_dst, src.data[0], src.width * src.height);
        std::memcpy(chroma_dst, src.data[1], (src.width * src.height) / 2);
    }

    if (src.height != dst_height) {
        // Extend the data vertically to the crop space
        const auto ly = src.height - 1;
        for (u32 y = src.height; y < dst_height; ++y) {
            std::memcpy(luma_dst + y * dst_pitch, src.data[0] + ly * src.linesize[0], src.width);
        }
        // src.height >= 2 is guaranteed above, so this can no longer underflow.
        const u32 cy = (src.height / 2) - 1;
        for (u32 y = src.height / 2; y < dst_height / 2; ++y) {
            std::memcpy(chroma_dst + y * dst_pitch, src.data[1] + cy * src.linesize[1], src.width);
        }
    }

    return true;
}

} // namespace Libraries::Videodec
