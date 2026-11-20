// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_GLOBAL_CONTEXT_H
#define CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_GLOBAL_CONTEXT_H

#include <asmjit/core.h>

#include "../../JitRuntimeBindings.h"
#include "../../LazyCalls/FunctionRegistry.h"
#include "../../LazyCalls/FunctionResolver.h"
#include "../../LazyCalls/LazyCallSiteRegistry.h"

namespace Cangjie::JIT::X86_64 {
class GlobalContext final {
public:
    GlobalContext() noexcept : allocatorParams(MakeAllocatorParams()), runtime(&allocatorParams) {}
    ~GlobalContext()
    {
        runtimeBindings.Shutdown();
    }

    BindingResult ConfigureRuntime(const RuntimeConfig& config)
    {
        const bool previousStackGrowth = stackGrowthEnabled;
        stackGrowthEnabled = std::get<4>(config);
        auto result = runtimeBindings.Initialize(config);
        if (!result.empty()) {
            stackGrowthEnabled = previousStackGrowth;
        }
        return result;
    }

    JitRuntimeBindings& GetRuntimeBindings() noexcept
    {
        return runtimeBindings;
    }

    bool IsStackGrowthEnabled() const noexcept
    {
        return stackGrowthEnabled;
    }


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
    LazyCallSiteRegistry& GetLazyCallSiteRegistry() noexcept
    {
        return lazyCallSites;
    }


private:
    static asmjit::JitAllocator::CreateParams MakeAllocatorParams() noexcept
    {
        asmjit::JitAllocator::CreateParams params{};
        params.options = asmjit::JitAllocatorOptions::kUseDualMapping;
        return params;
    }

    asmjit::JitAllocator::CreateParams allocatorParams;
    asmjit::JitRuntime runtime;
    FunctionRegistry functionRegistry;
    LazyCallSiteRegistry lazyCallSites;
    FunctionResolver functionResolver;
    JitRuntimeBindings runtimeBindings;
    bool stackGrowthEnabled = false;
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_GLOBAL_CONTEXT_H
