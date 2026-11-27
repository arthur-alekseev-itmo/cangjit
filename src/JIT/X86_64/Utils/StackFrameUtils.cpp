// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "StackFrameUtils.h"

#include <limits>

#include "../JitAssert.h"

namespace Cangjie::JIT::X86_64 {
namespace {
constexpr size_t SLOT_SIZE = sizeof(uint64_t);
constexpr size_t STACK_ALIGNMENT = 16;
constexpr size_t INT32_MAX_SLOTS = (static_cast<size_t>(std::numeric_limits<int32_t>::max()) + 1) / SLOT_SIZE;
}

uint32_t StackFrameUtils::GetFrameSize(size_t slotCount) noexcept
{
    JIT_ASSERT(slotCount < INT32_MAX_SLOTS, "stack frame is too large");
    return static_cast<uint32_t>((slotCount * SLOT_SIZE + STACK_ALIGNMENT - 1) & ~(STACK_ALIGNMENT - 1));
}

int32_t StackFrameUtils::GetSlotOffset(size_t slot, size_t slotCount) noexcept
{
    // The runtime uses the first slot below RBP to recover the function entry.
    JIT_ASSERT(slot < slotCount && slot + 2 < INT32_MAX_SLOTS, "stack slot is out of range");
    return -static_cast<int32_t>((slot + 2) * SLOT_SIZE);
}

int32_t StackFrameUtils::GetIncomingArgumentOffset(size_t index) noexcept
{
    constexpr size_t BASE_OFFSET = 2 * SLOT_SIZE;
    JIT_ASSERT(BASE_OFFSET + index * SLOT_SIZE < static_cast<size_t>(std::numeric_limits<int32_t>::max()),
        "incoming argument offset is out of range");
    return static_cast<int32_t>(BASE_OFFSET + index * SLOT_SIZE);
}
} // namespace Cangjie::JIT::X86_64
