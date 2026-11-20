// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "X86_64Compiler.h"

#include "Compilers/Function/CompileFunction.h"
#include "Compilers/Function/CompileFunctionStub.h"
#include "Compilers/Context/GlobalContext.h"
#include "cangjie/JIT/JitSymbols.h"
#include <Cangjie.h>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <utility>

#include "cangjie/CHIR/IR/Type/Type.h"
#include "cangjie/CHIR/IR/Value/Value.h"

#include "cangjie/CHIR/IR/Package.h"
#include "cangjie/Mangle/MangleUtils.h"

namespace Cangjie::JIT::X86_64 {
X86_64Compiler::X86_64Compiler() : globalContext(std::make_unique<GlobalContext>()) {}

X86_64Compiler::~X86_64Compiler() = default;
void X86_64Compiler::SetSidecar(std::string path, std::vector<const CHIR::Function*> functions)
{
    assert(!runtimeConfigured);
    sidecarPath = std::move(path);
    sidecarFunctions = std::move(functions);
}



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
        if (!runtimeConfigured) {
            const char* home = std::getenv("CANGJIE_HOME");
            if (!home || !*home) {
                std::cerr << "JIT: CANGJIE_HOME is required to locate the Cangjie runtime\n";
                output.setstate(std::ios::badbit);
                return;
            }
            const char* stackGrowth = std::getenv("cjStackGrow");
            if (!stackGrowth) {
#ifdef CANGJIE_JIT_NO_STACK_GROWTH
                const char* defaultStackGrowth = "0";
#else
                const char* defaultStackGrowth = "1";
#endif
                if (setenv("cjStackGrow", defaultStackGrowth, 0) != 0) {
                    std::cerr << "JIT: cannot configure runtime stack growth\n";
                    output.setstate(std::ios::badbit);
                    return;
                }
                stackGrowth = std::getenv("cjStackGrow");
            }
            if (!stackGrowth || (std::strcmp(stackGrowth, "0") != 0 && std::strcmp(stackGrowth, "1") != 0)) {
                std::cerr << "JIT: cjStackGrow must be 0 or 1\n";
                output.setstate(std::ios::badbit);
                return;
            }
            const std::string libDir = std::string(home) + "/runtime/lib/linux_x86_64_cjnative/";
            if (sidecarPath.empty()) {
                std::cerr << "JIT: package metadata sidecar is required\n";
                output.setstate(std::ios::badbit);
                return;
            }
            const RuntimeParam params{};
            const auto bindSidecar = [this]() -> BindingResult {
                auto& bindings = globalContext->GetRuntimeBindings();
                for (const auto* sidecarFunction : sidecarFunctions) {
                    if (!sidecarFunction) {
                        return "JIT sidecar contains an invalid function";
                    }
                    const auto identifier = sidecarFunction->GetIdentifierWithoutPrefix();
                    auto symbol = bindings.Resolve(JitFunctionSlotName(identifier));
                    const auto* slot = std::get_if<void*>(&symbol);
                    if (!slot || !*slot) {
                        return "JIT sidecar function slot not found: " + identifier;
                    }
                    const auto entry = CompileFunctionStub::Compile(*globalContext, sidecarFunction);
                    if (!entry) {
                        return "cannot create a JIT entry stub for " + identifier;
                    }
                    *static_cast<void**>(*slot) = reinterpret_cast<void*>(entry);
                }
                return {};
            };
            const RuntimeConfig config{libDir + "libcangjie-runtime.so",
                {libDir + "libcangjie-std-core.so"}, &params, true, std::strcmp(stackGrowth, "1") == 0,
                sidecarPath, bindSidecar};
            const auto error = globalContext->ConfigureRuntime(config);
            if (!error.empty()) {
                std::cerr << "JIT runtime initialization failed: " << error << '\n';
                output.setstate(std::ios::badbit);
                return;
            }
            runtimeConfigured = true;
        }
        const auto result = CompileFunction(*globalContext, *function).CompileToAddress(&output);
        if (const auto* error = std::get_if<CompilationError>(&result)) {
            std::cerr << "JIT compilation failed: " << error->message << '\n';
            output.setstate(std::ios::badbit);
            return;
        }
        auto& bindings = globalContext->GetRuntimeBindings();
        auto runSymbol = bindings.Resolve("RunCJTask");
        auto getSymbol = bindings.Resolve("GetTaskRet");
        auto releaseSymbol = bindings.Resolve("ReleaseHandle");
        const auto* runAddress = std::get_if<void*>(&runSymbol);
        const auto* getAddress = std::get_if<void*>(&getSymbol);
        const auto* releaseAddress = std::get_if<void*>(&releaseSymbol);
        if (!runAddress || !getAddress || !releaseAddress) {
            const auto& failed = !runAddress ? runSymbol : (!getAddress ? getSymbol : releaseSymbol);
            std::cerr << "JIT runtime symbol resolution failed: " << std::get<std::string>(failed) << '\n';
            output.setstate(std::ios::badbit);
            return;
        }
        const auto run = reinterpret_cast<decltype(&RunCJTask)>(*runAddress);
        const auto get = reinterpret_cast<decltype(&GetTaskRet)>(*getAddress);
        const auto release = reinterpret_cast<decltype(&ReleaseHandle)>(*releaseAddress);
        const auto entry = reinterpret_cast<CJTaskFunc>(std::get<CodeAddress>(result));
        const auto handle = run(entry, nullptr);
        if (!handle) {
            std::cerr << "JIT: RunCJTask failed\n";
            output.setstate(std::ios::badbit);
            return;
        }
        void* value = nullptr;
        const int status = get(handle, &value);
        release(handle);
        if (status != E_OK) {
            std::cerr << "JIT: GetTaskRet failed with error " << status << '\n';
            output.setstate(std::ios::badbit);
            return;
        }
        static_assert(sizeof(value) == sizeof(int64_t));
        int64_t mainResult;
        std::memcpy(&mainResult, &value, sizeof(mainResult));
        output << "JIT: main returned " << mainResult << '\n';
        return;
    }
}
} // namespace Cangjie::JIT::X86_64
