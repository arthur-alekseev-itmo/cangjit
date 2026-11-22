// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_TYPE_INFO_EMITTER_H
#define CANGJIE_JIT_X86_64_TYPE_INFO_EMITTER_H

#include <asmjit/x86.h>

namespace Cangjie::CHIR { class Type; }

namespace Cangjie::JIT::X86_64 {
class FunctionContext;

class TypeInfoEmitter final {
public:
    explicit TypeInfoEmitter(FunctionContext& context) noexcept : context(context) {}
    void* GetConcreteTypeInfo(const CHIR::Type& type);
    bool LoadTypeInfo(const CHIR::Type& type, const asmjit::x86::Gp& target);

private:
    FunctionContext& context;
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_TYPE_INFO_EMITTER_H
