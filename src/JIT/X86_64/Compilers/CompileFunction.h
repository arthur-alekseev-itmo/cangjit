// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_COMPILERS_COMPILE_FUNCTION_H
#define CANGJIE_JIT_X86_64_COMPILERS_COMPILE_FUNCTION_H

#include <iosfwd>

namespace Cangjie::CHIR {
class Function;
}

namespace Cangjie::JIT::X86_64 {

/// Runs the x86-64 compilation pipeline for one CHIR function.
class CompileFunction final {
public:
    explicit CompileFunction(const CHIR::Function& function) noexcept;

    void Compile(std::ostream& output) const;

private:
    const CHIR::Function& function;
};

} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_COMPILERS_COMPILE_FUNCTION_H
