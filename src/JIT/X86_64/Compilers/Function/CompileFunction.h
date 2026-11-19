// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_COMPILERS_COMPILE_FUNCTION_H
#define CANGJIE_JIT_X86_64_COMPILERS_COMPILE_FUNCTION_H

#include <iosfwd>

#include "../../Types.h"

namespace Cangjie::CHIR {
class Function;
}

namespace Cangjie::JIT::X86_64 {
class FunctionContext;
class GlobalContext;


class CompileFunction final {
public:
    CompileFunction(GlobalContext& globalContext, const CHIR::Function& function) noexcept;

    CompilationResult CompileToAddress(std::ostream* output = nullptr) const;
    void Compile(FunctionContext& context) const;

private:
    void MakePrologue(FunctionContext& context) const;
    void MakeEpilogue(FunctionContext& context) const;

    GlobalContext& globalContext;
    const CHIR::Function& function;
};

} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_COMPILERS_COMPILE_FUNCTION_H
