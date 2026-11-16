// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "X86_64Compiler.h"

#include "Compilers/CompileFunction.h"

#include "cangjie/CHIR/IR/Package.h"
#include "cangjie/Mangle/MangleUtils.h"

namespace Cangjie::JIT::X86_64 {
void X86_64Compiler::Compile(const CHIR::Package& package, std::ostream& output) const
{
    for (const auto* function : package.GetGlobalFuncsWithBody()) {
        if (function->GetIdentifierWithoutPrefix() != USER_MAIN_MANGLED_NAME) {
            continue;
        }
        CompileFunction(*function).Compile(output);
        return;
    }
}
} // namespace Cangjie::JIT::X86_64
