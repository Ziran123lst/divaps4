// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)
// Copyright © 2015-2023 The Khronos Group Inc.
// Copyright © 2015-2023 Valve Corporation
// Copyright © 2015-2023 LunarG, Inc.

#pragma once

#include "video_core/renderer_vulkan/vk_common.h"

namespace VideoCore {

/// Returns true if the two formats are compatible according to Vulkan's format compatibility rules
bool IsVulkanFormatCompatible(vk::Format base, vk::Format view);

/// Returns true if the format has an sRGB variant, i.e. sampling it applies a gamma decode and
/// rendering to it applies a gamma encode in the fixed function units.
bool IsSrgbFormat(vk::Format format);

/// Returns the format with the requested sRGB-ness. Formats without an sRGB variant (float,
/// integer, depth, BC4/BC5/BC6...) are returned unchanged, as they carry no gamma semantics.
/// This is used to keep a view's gamma interpretation in sync with how the image was written
/// instead of letting it depend on allocation history.
vk::Format ToggleSrgb(vk::Format format, bool srgb);

} // namespace VideoCore
