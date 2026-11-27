// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_COMPILERS_COMPILE_DEBUG_H
#define CANGJIE_JIT_X86_64_COMPILERS_COMPILE_DEBUG_H

namespace Cangjie::CHIR {
class Debug;
}

namespace Cangjie::JIT::X86_64 {
class FunctionContext;

class CompileDebug final {
public:
    static void Compile(FunctionContext& ctx, const CHIR::Debug& expr);
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_COMPILERS_COMPILE_DEBUG_H
