// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_UTILS_MEMORY_UTILS_H
#define CANGJIE_JIT_X86_64_UTILS_MEMORY_UTILS_H

#include <asmjit/x86.h>

namespace Cangjie::CHIR {
class Value;
}

namespace Cangjie::JIT::X86_64 {
class FunctionContext;
class Location;

/// Emits scalar moves. Stack locations are eight-byte frame-pointer-relative slots.
class MemoryUtils final {
public:
    static bool LoadValue(FunctionContext& context, const CHIR::Value* value, const asmjit::x86::Gp& target);
    static bool StoreValue(FunctionContext& context, const CHIR::Value* value, const asmjit::x86::Gp& source);

    /// Moves between stack slots and 64-bit registers. For a stack-to-stack move,
    /// scratch must be a caller-owned 64-bit register other than RBP or RSP.
    static bool Move(FunctionContext& context, const Location& destination, const Location& source,
        const asmjit::x86::Gp& scratch);
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_UTILS_MEMORY_UTILS_H
