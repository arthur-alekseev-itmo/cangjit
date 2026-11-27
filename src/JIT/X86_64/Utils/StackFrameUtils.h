// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_UTILS_STACK_FRAME_UTILS_H
#define CANGJIE_JIT_X86_64_UTILS_STACK_FRAME_UTILS_H

#include <cstddef>
#include <cstdint>

namespace Cangjie::JIT::X86_64 {
class StackFrameUtils final {
public:
    static uint32_t GetFrameSize(size_t slotCount) noexcept;
    static int32_t GetSlotOffset(size_t slot, size_t slotCount) noexcept;
    static int32_t GetIncomingArgumentOffset(size_t index) noexcept;
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_UTILS_STACK_FRAME_UTILS_H
