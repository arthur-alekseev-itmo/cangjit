// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "X86_64Compiler.h"

#include "Compilers/Function/CompileFunction.h"
#include "Compilers/Context/GlobalContext.h"
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
X86_64Compiler::X86_64Compiler() = default;

X86_64Compiler::X86_64Compiler(std::vector<std::string> imports, std::vector<std::string> searchPaths)
    : importPaths(std::move(imports)), librarySearchPaths(std::move(searchPaths)) {}

X86_64Compiler::~X86_64Compiler() = default;

Result<std::unique_ptr<X86_64Compiler>, RuntimeError> X86_64Compiler::Create(const RuntimeConfig& config)
{
    auto runtime = JitRuntime::Create(config);
    if (!runtime) {
        return Result<std::unique_ptr<X86_64Compiler>, RuntimeError>::Failure(std::move(runtime).Error());
    }
    auto compiler = std::make_unique<X86_64Compiler>();
    compiler->jitRuntime = std::move(runtime).Value();
    compiler->globalContext = std::make_unique<GlobalContext>(*compiler->jitRuntime);
    return Result<std::unique_ptr<X86_64Compiler>, RuntimeError>::Success(std::move(compiler));
}

Result<void, RuntimeError> X86_64Compiler::Close() const
{
    closed = true;
    return globalContext ? globalContext->Close() : Result<void, RuntimeError>::Success();
}

void X86_64Compiler::Run(const CHIR::Package& package, std::ostream& output) const
{
    if (closed) {
        ReportError(output, "JIT: compiler is closed");
        return;
    }
    auto function = FindUserMain(package);
    if (!function || !ValidateUserMain(*function, output) ||
        !ValidatePackageInitializers(package, output) || !ConfigureRuntime(output)) {
        ReportError(output, "JIT: entrypoint main is not found or is incorrect");
        return;
    }
    const auto address = CompileUserMain(*function, output);
    if (!address) {
        return;
    }
    RunUserMain(*address, output);
}

const CHIR::Function* X86_64Compiler::FindUserMain(const CHIR::Package& package)
{
    for (auto function : package.GetGlobalFuncsWithBody()) {
        if (function->GetIdentifierWithoutPrefix() == USER_MAIN_MANGLED_NAME) {
            return function;
        }
    }
    return nullptr;
}

bool X86_64Compiler::ValidateUserMain(const CHIR::Function& function, std::ostream& output)
{
    if (!function.GetParams().empty() || !function.GetReturnType() ||
        function.GetReturnType()->GetTypeKind() != CHIR::Type::TYPE_INT64) {
        return ReportError(output, "JIT: unsupported user main signature");
    }
    return true;
}

bool X86_64Compiler::ValidatePackageInitializers(const CHIR::Package& package, std::ostream& output)
{
    // Native entry runs package initializers before user.main.
    for (auto global : package.GetGlobalVars()) {
        if (!global || global->IsImportedVar() || !global->GetInitFunc()) {
            continue;
        }
        return ReportError(output, "JIT: package-local global initialization is not supported");
    }
    return true;
}

bool X86_64Compiler::ReportError(std::ostream& output, std::string_view message)
{
    std::cerr << message << '\n';
    output.setstate(std::ios::badbit);
    return false;
}

bool X86_64Compiler::ConfigureStackGrowth(bool& enabled, std::ostream& output)
{
    const char* stackGrowth = std::getenv("cjStackGrow");
    if (!stackGrowth) {
#ifdef CANGJIE_JIT_NO_STACK_GROWTH
        const char* defaultStackGrowth = "0";
#else
        const char* defaultStackGrowth = "1";
#endif
        if (setenv("cjStackGrow", defaultStackGrowth, 0) != 0) {
            return ReportError(output, "JIT: cannot configure runtime stack growth");
        }
        stackGrowth = std::getenv("cjStackGrow");
    }
    if (!stackGrowth || (std::strcmp(stackGrowth, "0") != 0 && std::strcmp(stackGrowth, "1") != 0)) {
        return ReportError(output, "JIT: cjStackGrow must be 0 or 1");
    }
    enabled = std::strcmp(stackGrowth, "1") == 0;
    return true;
}

// FIX: Maybe im wrong, but doesn't it belong to some runtime class? Or better yet runtime loader
bool X86_64Compiler::ConfigureRuntime(std::ostream& output) const
{
    if (globalContext) {
        return true;
    }
    const char* home = std::getenv("CANGJIE_HOME");
    if (!home || !*home) {
        return ReportError(output, "JIT: CANGJIE_HOME is required to locate the Cangjie runtime");
    }
    bool stackGrowth;
    if (!ConfigureStackGrowth(stackGrowth, output)) {
        return false;
    }
    const std::string libDir = std::string(home) + "/runtime/lib/linux_x86_64_cjnative/";
    const RuntimeParam params{};
    auto paths = librarySearchPaths;
    paths.push_back(std::string(home) + "/lib/linux_x86_64_cjnative");
    const RuntimeConfig config{libDir + "libcangjie-runtime.so",
        {libDir + "libcangjie-std-core.so"}, &params, true, stackGrowth, importPaths, std::move(paths)};
    auto runtime = JitRuntime::Create(config);
    if (!runtime) {
        return ReportError(output, "JIT runtime initialization failed: " + FormatRuntimeError(runtime.Error()));
    }
    jitRuntime = std::move(runtime).Value();
    globalContext = std::make_unique<GlobalContext>(*jitRuntime);
    return true;
}

std::optional<std::uintptr_t> X86_64Compiler::CompileUserMain(const CHIR::Function& function, std::ostream& output) const
{
    const auto result = CompileFunction(*globalContext, function).CompileToAddress();
    if (!result) {
        ReportError(output, "JIT compilation failed: " + result.Error().message);
        return std::nullopt;
    }
    return result.Value();
}


void X86_64Compiler::RunUserMain(std::uintptr_t address, std::ostream& output) const
{
    auto& bindings = globalContext->GetRuntimeBindings();
    const auto run = bindings.GetRunTask();
    const auto get = bindings.GetTaskReturn();
    const auto release = bindings.GetReleaseTask();
    if (!run || !get || !release) {
        const auto& error = !run ? run.Error() : !get ? get.Error() : release.Error();
        ReportError(output, "JIT runtime symbol resolution failed: " + FormatRuntimeError(error));
        return;
    }
    const auto entry = reinterpret_cast<CJTaskFunc>(address);
    const auto handle = run.Value()(entry, nullptr);
    if (!handle) {
        ReportError(output, "JIT: RunCJTask failed");
        return;
    }
    void* value = nullptr;
    const int status = get.Value()(handle, &value);
    release.Value()(handle);
    if (status != E_OK) {
        ReportError(output, "JIT: GetTaskRet failed with error " + std::to_string(status));
        return;
    }
    static_assert(sizeof(value) == sizeof(int64_t));
    int64_t mainResult;
    std::memcpy(&mainResult, &value, sizeof(mainResult));
    output << "JIT: main returned " << mainResult << '\n';
}
} // namespace Cangjie::JIT::X86_64
