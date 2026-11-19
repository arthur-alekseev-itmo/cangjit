// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_REGISTER_ALLOCATION_REGISTER_ALLOCATOR_H
#define CANGJIE_JIT_X86_64_REGISTER_ALLOCATION_REGISTER_ALLOCATOR_H

#include "RegisterAllocation.h"

namespace Cangjie::JIT::X86_64 {

class InterferenceGraph;
class ValueNumeration;

class RegisterAllocator {
public:
    virtual ~RegisterAllocator() = default;

    virtual RegisterAllocation Allocate(
        const ValueNumeration& numeration, const InterferenceGraph& interferenceGraph) const = 0;
};

} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_REGISTER_ALLOCATION_REGISTER_ALLOCATOR_H
