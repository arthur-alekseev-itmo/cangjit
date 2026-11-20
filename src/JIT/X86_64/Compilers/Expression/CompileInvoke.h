// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_COMPILERS_COMPILE_INVOKE_H
#define CANGJIE_JIT_X86_64_COMPILERS_COMPILE_INVOKE_H

namespace Cangjie::CHIR {
class Invoke;
class TryInvoke;
class InvokeStatic;
class TryInvokeStatic;
}

namespace Cangjie::JIT::X86_64 {
class FunctionContext;

class CompileInvoke final {
public:
    static void Compile(FunctionContext& context, const CHIR::Invoke& expression);
    static void Compile(FunctionContext& context, const CHIR::TryInvoke& expression);
    static void Compile(FunctionContext& context, const CHIR::InvokeStatic& expression);
    static void Compile(FunctionContext& context, const CHIR::TryInvokeStatic& expression);
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_COMPILERS_COMPILE_INVOKE_H
