// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_COMPILERS_COMPILE_CONSTANT_H
#define CANGJIE_JIT_X86_64_COMPILERS_COMPILE_CONSTANT_H

namespace Cangjie::CHIR {
class Constant;
}

namespace Cangjie::JIT::X86_64 {
class FunctionContext;

class CompileConstant final {
public:
    static void Compile(FunctionContext& context, const CHIR::Constant& expression);
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_COMPILERS_COMPILE_CONSTANT_H
