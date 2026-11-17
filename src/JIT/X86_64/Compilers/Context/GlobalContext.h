// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_GLOBAL_CONTEXT_H
#define CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_GLOBAL_CONTEXT_H

#include <asmjit/core.h>

#include "../../LazyCalls/FunctionRegistry.h"
#include "../../LazyCalls/FunctionResolver.h"

namespace Cangjie::JIT::X86_64 {
class GlobalContext final {
public:
    asmjit::JitRuntime& GetRuntime() noexcept
    {
        return runtime;
    }

    FunctionRegistry& GetFunctionRegistry() noexcept
    {
        return functionRegistry;
    }

    FunctionResolver& GetFunctionResolver() noexcept
    {
        return functionResolver;
    }

private:
    asmjit::JitRuntime runtime;
    FunctionRegistry functionRegistry;
    FunctionResolver functionResolver;
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_GLOBAL_CONTEXT_H
