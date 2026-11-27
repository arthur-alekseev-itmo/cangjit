// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_UTILS_MEMORY_UTILS_H
#define CANGJIE_JIT_X86_64_UTILS_MEMORY_UTILS_H

#include <asmjit/x86.h>

#include <cstdint>

namespace Cangjie::CHIR {
class Type;
}

namespace Cangjie::JIT::X86_64 {
class FunctionContext;
class Location;

// Emitting a move records AsmJit errors on the context; the sticky error is
// checked once after the function body is emitted.
class MemoryUtils final {
public:
    static void Move(FunctionContext& context, const Location& destination, const Location& source,
        const CHIR::Type& type, const asmjit::x86::Gp& scratch);
    static void StoreConstant(FunctionContext& context, uint64_t bits, const Location& destination,
        const CHIR::Type& type, const asmjit::x86::Gp& scratch);
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_UTILS_MEMORY_UTILS_H
