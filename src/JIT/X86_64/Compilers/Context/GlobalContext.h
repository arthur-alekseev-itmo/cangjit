// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_GLOBAL_CONTEXT_H
#define CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_GLOBAL_CONTEXT_H

#include <asmjit/core.h>
#include <cstdio>
#include "DebugConfig.h"


#include "../../JitRuntime.h"
#include "../../LazyCalls/FunctionRegistry.h"
#include "../../LazyCalls/FunctionResolver.h"
#include "../../LazyCalls/LazyCallSiteRegistry.h"
#include "cangjie/CHIR/IR/CHIRBuilder.h"
#include "cangjie/CHIR/IR/CHIRContext.h"

namespace Cangjie::JIT::X86_64 {
class GlobalContext final {
public:
    explicit GlobalContext(JitRuntime& jitRuntime)
        : jitRuntime(jitRuntime), allocatorParams(MakeAllocatorParams()), runtime(&allocatorParams)
    {
        logger.addFlags(asmjit::FormatFlags::kMachineCode | asmjit::FormatFlags::kHexImms);
    }
    ~GlobalContext() { jitRuntime.CloseOrTerminate(); }

    // The host must quiesce generated code and tasks before closing.
    Result<void, RuntimeError> Close()
    {
        return jitRuntime.Close();
    }


    JitRuntimeBindings& GetRuntimeBindings() noexcept
    {
        return jitRuntime.GetBindings();
    }

    bool IsStackGrowthEnabled() const noexcept
    {
        return jitRuntime.IsStackGrowthEnabled();
    }

    bool IsDebugEnabled(FuncPrintMode mode) const noexcept
    {
        return IsEnabled(debugConfig.func_print_mode, mode);
    }

    asmjit::Logger* GetLogger() noexcept
    {
        return IsDebugEnabled(FuncPrintMode::ASM) ? &logger : nullptr;
    }

    CHIR::CHIRBuilder& GetCHIRBuilder() noexcept
    {
        return chirBuilder;
    }


    StringLiteralPool& GetStringLiteralPool() noexcept
    {
        return jitRuntime.GetStringLiteralPool();
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

    JitRuntime& jitRuntime;
    CHIR::CHIRContext chirContext;
    CHIR::CHIRBuilder chirBuilder{chirContext};
    asmjit::JitAllocator::CreateParams allocatorParams;
    asmjit::JitRuntime runtime;
    FunctionRegistry functionRegistry;
    LazyCallSiteRegistry lazyCallSites;
    FunctionResolver functionResolver;
    asmjit::FileLogger logger{stderr};
    DebugConfig debugConfig{DEFAULT_DEBUG_CONFIG};
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_GLOBAL_CONTEXT_H
