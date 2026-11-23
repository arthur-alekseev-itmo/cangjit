// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_JIT_RUNTIME_BINDINGS_H
#define CANGJIE_JIT_X86_64_JIT_RUNTIME_BINDINGS_H

#include <Cangjie.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "RuntimeConfig.h"
#include "RuntimeError.h"
#include "../X86_64/Types.h"


namespace Cangjie::JIT::X86_64 {
using SymbolName = std::string_view;



using BindingResult = Result<void, RuntimeError>;
using SymbolResult = Result<void*, RuntimeError>;


class JitRuntimeBindings {
public:
    JitRuntimeBindings() = default;
    JitRuntimeBindings(const JitRuntimeBindings&) = delete;
    JitRuntimeBindings& operator=(const JitRuntimeBindings&) = delete;

    BindingResult Initialize(const RuntimeConfig& config);
    BindingResult EnsurePackage(std::string_view package);
    SymbolResult Resolve(SymbolName name);
#define JIT_RUNTIME_FUNCTION(name, type, symbol, required) Result<type, RuntimeError> Get##name();
#include "RuntimeFunctions.inc"
#undef JIT_RUNTIME_FUNCTION
    // The host must first quiesce generated code and tasks. For a borrowed
    // runtime it must also ensure no heap objects or GC work can reference
    // the loaded libraries. Calls are externally serialized.
    Result<void, RuntimeError> Close();
    bool HasFailedClose() const noexcept { return closeFailed; }

private:
    static BindingResult ValidateLibraries(const std::vector<std::string>& requestedLibraries);
    static std::string GetRuntimeDirectory(const std::string& runtimePath);
    BindingResult LoadRuntime(const std::string& runtimePath);
    BindingResult LoadRequiredSymbols();
    BindingResult InitializeRuntime(const ::RuntimeParam* params, bool owner);
    Result<void, RuntimeError> FinalizeRuntime();
    Result<void, RuntimeError> UnloadLibraries();
    BindingResult FailInitialization(RuntimeError error);
    BindingResult LoadLibraries(const std::vector<std::string>& requestedLibraries);
    BindingResult LoadLibraryPath(const std::string& path);
    bool IsLibraryLoaded(std::string_view name) const noexcept;
    std::optional<std::string> FindPackageLibrary(const std::string& name) const;

#define JIT_RUNTIME_FUNCTION(name, type, symbol, required) type name = nullptr;
#include "RuntimeFunctions.inc"
#undef JIT_RUNTIME_FUNCTION
    void* runtime = nullptr;
    std::string runtimeDirectory;
    std::vector<std::string> importPaths;
    std::vector<std::string> librarySearchPaths;
    std::vector<std::string> libraries;
    std::unordered_map<std::string, void*> symbols;
    bool ownsRuntime = false;
    bool initialized = false;
    bool closeFailed = false;
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_JIT_RUNTIME_BINDINGS_H
