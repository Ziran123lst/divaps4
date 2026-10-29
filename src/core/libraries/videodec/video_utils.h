// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

struct AVFrame;

namespace Libraries::Videodec {

/// Copies an NV12 frame into a guest frame buffer of \a max_size bytes. Returns false when the
/// frame is malformed or does not fit, in which case nothing is written.
bool CopyNV12Data(u8* dst, u64 max_size, const AVFrame& src);

}
