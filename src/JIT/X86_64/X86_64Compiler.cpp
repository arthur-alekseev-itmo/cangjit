// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "X86_64Compiler.h"

#include "Compilers/Function/CompileFunction.h"
#include "Compilers/Context/GlobalContext.h"
#include <iostream>

#include "cangjie/CHIR/IR/Type/Type.h"
#include "cangjie/CHIR/IR/Value/Value.h"

#include "cangjie/CHIR/IR/Package.h"
#include "cangjie/Mangle/MangleUtils.h"

namespace Cangjie::JIT::X86_64 {
X86_64Compiler::X86_64Compiler() : globalContext(std::make_unique<GlobalContext>()) {}

X86_64Compiler::~X86_64Compiler() = default;


void X86_64Compiler::Compile(const CHIR::Package& package, std::ostream& output) const
{
    for (const auto* function : package.GetGlobalFuncsWithBody()) {
        if (function->GetIdentifierWithoutPrefix() != USER_MAIN_MANGLED_NAME) {
            continue;
        }
        if (!function->GetParams().empty() || !function->GetReturnType() ||
            function->GetReturnType()->GetTypeKind() != CHIR::Type::TYPE_INT64) {
            std::cerr << "JIT: unsupported user main signature\n";
            output.setstate(std::ios::badbit);
            return;
        }
        const auto result = CompileFunction(*globalContext, *function).CompileToAddress(&output);
        if (const auto* error = std::get_if<CompilationError>(&result)) {
            std::cerr << "JIT compilation failed: " << error->message << '\n';
            output.setstate(std::ios::badbit);
            return;
        }
        const auto entry = reinterpret_cast<int64_t (*)()>(std::get<CodeAddress>(result));
        output << "JIT: main returned " << entry() << '\n';
        return;
    }
}
} // namespace Cangjie::JIT::X86_64
