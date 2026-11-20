// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_UTILS_MEMORY_UTILS_H
#define CANGJIE_JIT_X86_64_UTILS_MEMORY_UTILS_H

#include <asmjit/x86.h>

#include <cstddef>
#include <cstdint>

namespace Cangjie::CHIR {
class Type;
class Value;
}

namespace Cangjie::JIT::X86_64 {
class FunctionContext;
class Location;

class MemoryUtils final {
public:
    static bool LoadValue(FunctionContext& context, const CHIR::Value* value, const asmjit::x86::Gp& target);
    static bool StoreValue(FunctionContext& context, const CHIR::Value* value, const asmjit::x86::Gp& source);
    // The scratch GP register is clobbered. Float16 literals are not supported.
    static bool LoadFloatValue(FunctionContext& context, const CHIR::Value* value,
        const asmjit::x86::Xmm& target, const asmjit::x86::Gp& scratch);
    static bool StoreFloatValue(FunctionContext& context, const CHIR::Value* value,
        const asmjit::x86::Xmm& source, const asmjit::x86::Gp& scratch);
    static void ReserveStack(FunctionContext& context, uint32_t size);
    static void ReleaseStack(FunctionContext& context, uint32_t size);
    static bool StoreOutgoingArgument(FunctionContext& context, const CHIR::Value* value, size_t index,
        uint32_t stackSize);
    static bool PlaceTypeInfo(FunctionContext& context, const CHIR::Type* type, const Location& location,
        uint32_t stackSize);
    static bool ZeroStackValueAndLoadAddress(
        FunctionContext& context, const CHIR::Value* value, const asmjit::x86::Gp& target);

    static bool Move(FunctionContext& context, const Location& destination, const Location& source,
        const asmjit::x86::Gp& scratch);
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_UTILS_MEMORY_UTILS_H
