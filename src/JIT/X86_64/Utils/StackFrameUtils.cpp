// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "StackFrameUtils.h"

#include <limits>

namespace Cangjie::JIT::X86_64 {
namespace {
constexpr size_t SLOT_SIZE = sizeof(uint64_t);
constexpr size_t STACK_ALIGNMENT = 16;
}

std::optional<uint32_t> StackFrameUtils::GetFrameSize(size_t slotCount) noexcept
{
    if (slotCount > (static_cast<size_t>(std::numeric_limits<int32_t>::max()) - (STACK_ALIGNMENT - 1)) / SLOT_SIZE) {
        return std::nullopt;
    }
    return static_cast<uint32_t>((slotCount * SLOT_SIZE + STACK_ALIGNMENT - 1) & ~(STACK_ALIGNMENT - 1));
}

std::optional<int32_t> StackFrameUtils::GetSlotOffset(size_t slot, size_t slotCount) noexcept
{
    // The runtime uses the first slot below RBP to recover the function entry.
    if (slot >= slotCount || slot > static_cast<size_t>(std::numeric_limits<int32_t>::max()) / SLOT_SIZE - 2) {
        return std::nullopt;
    }
    return -static_cast<int32_t>((slot + 2) * SLOT_SIZE);
}
} // namespace Cangjie::JIT::X86_64
