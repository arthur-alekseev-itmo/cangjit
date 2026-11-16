// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_REGISTER_ALLOCATION_TRIVIAL_STACK_REGALLOC_H
#define CANGJIE_JIT_X86_64_REGISTER_ALLOCATION_TRIVIAL_STACK_REGALLOC_H

#include "RegisterAllocator.h"

namespace Cangjie::JIT::X86_64 {

class TrivialStackRegalloc final : public RegisterAllocator {
public:
    RegisterAllocation Allocate(
        const ValueNumeration& numeration, const InterferenceGraph& interferenceGraph) const override;
};

} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_REGISTER_ALLOCATION_TRIVIAL_STACK_REGALLOC_H
