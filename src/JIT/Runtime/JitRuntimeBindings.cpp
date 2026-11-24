// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "JitRuntimeBindings.h"
#include "../X86_64/Compilers/Context/Logger.h"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <dlfcn.h>
#include <filesystem>
#include <utility>

namespace Cangjie::JIT::X86_64 {
namespace {
constexpr std::string_view STANDARD_PACKAGE_PREFIX = "std.";
constexpr std::string_view STANDARD_LIBRARY_PREFIX = "libcangjie-std-";
constexpr std::string_view SHARED_LIBRARY_SUFFIX = ".so";

std::vector<std::string> PackageLibraryNames(std::string_view package)
{
    if (package.substr(0, STANDARD_PACKAGE_PREFIX.size()) == STANDARD_PACKAGE_PREFIX) {
        return {std::string(STANDARD_LIBRARY_PREFIX) +
            std::string(package.substr(STANDARD_PACKAGE_PREFIX.size())) + std::string(SHARED_LIBRARY_SUFFIX)};
    }
    std::string name(package);
    const auto organization = name.find("::");
    std::string suffix;
    if (organization != std::string::npos) {
        suffix = "@" + name.substr(0, organization);
        name.erase(0, organization + 2);
    }
    std::vector<std::string> names{"lib" + name + suffix + std::string(SHARED_LIBRARY_SUFFIX)};
    const auto dot = name.find('.');
    if (dot != std::string::npos) {
        names.push_back("lib" + name.substr(0, dot) + suffix + std::string(SHARED_LIBRARY_SUFFIX));
    }
    return names;
}

std::optional<std::string> FindLibraryInDirectory(const std::string& directory, const std::string& name)
{
    const auto path = std::filesystem::path(directory.empty() ? "." : directory) / name;
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return std::nullopt;
    }
    return path.string();
}

std::optional<std::string> FindLibraryInDirectories(
    const std::vector<std::string>& directories, const std::string& name)
{
    for (const auto& directory : directories) {
        if (auto path = FindLibraryInDirectory(directory, name)) {
            return path;
        }
    }
    return std::nullopt;
}

std::optional<std::string> FindLibraryInEnvironment(const std::string& name)
{
    const char* environment = std::getenv("LIBRARY_PATH");
    std::string_view paths = environment ? environment : "";
    while (!paths.empty()) {
        const auto separator = paths.find(':');
        if (auto path = FindLibraryInDirectory(std::string(paths.substr(0, separator)), name)) {
            return path;
        }
        if (separator == std::string_view::npos) {
            break;
        }
        paths.remove_prefix(separator + 1);
    }
    return std::nullopt;
}

}

BindingResult JitRuntimeBindings::Initialize(const RuntimeConfig& config)
{
    if (runtime != nullptr) {
        return BindingResult::Failure({RuntimeErrorKind::AlreadyInitialized});
    }
    const auto& [runtimePath, requestedLibraries, params, owner, stackGrowth, imports, searchPaths] = config;
    // This is a host assertion, not a runtime switch. Consumers must pass the
    // same value to the metadata encoder; no public query exposes the mode.
    // Stack roots in metadata must match the runtime's stack-growth setting.
    (void)stackGrowth;
    if (runtimePath.empty() || (owner && params == nullptr)) {
        return BindingResult::Failure({RuntimeErrorKind::MissingRuntimeConfig});
    }
    if (auto result = ValidateLibraries(requestedLibraries); !result) {
        return result;
    }

    if (auto result = LoadRuntime(runtimePath); !result) {
        return result;
    }

    try {
        importPaths = imports;
        librarySearchPaths = searchPaths;
        if (auto result = LoadRequiredSymbols(); !result) {
            return FailInitialization(std::move(result).Error());
        }
        libraries.reserve(requestedLibraries.size());
        if (auto result = InitializeRuntime(params, owner); !result) {
            return FailInitialization(std::move(result).Error());
        }
        if (auto result = LoadLibraries(requestedLibraries); !result) {
            return FailInitialization(std::move(result).Error());
        }
        initialized = true;
        return BindingResult::Success();
    } catch (...) {
        auto cleanup = Close();
        if (!cleanup) {
            std::fputs("JIT runtime initialization cleanup failed\n", stderr);
            std::terminate();
        }
        throw;
    }
}

BindingResult JitRuntimeBindings::FailInitialization(RuntimeError error)
{
    auto cleanup = Close();
    if (!cleanup) {
        auto failure = std::move(cleanup).Error();
        error.cleanup = RuntimeFailure{failure.kind, failure.code, std::move(failure.detail)};
    }
    return BindingResult::Failure(std::move(error));
}

BindingResult JitRuntimeBindings::ValidateLibraries(const std::vector<std::string>& requestedLibraries)
{
    for (auto it = requestedLibraries.begin(); it != requestedLibraries.end(); ++it) {
        if (it->empty()) {
            return BindingResult::Failure({RuntimeErrorKind::EmptyLibraryPath});
        }
        const auto basename = it->substr(it->find_last_of('/') + 1);
        if (basename.empty()) {
            return BindingResult::Failure({RuntimeErrorKind::EmptyLibraryBasename});
        }
        for (auto previous = requestedLibraries.begin(); previous != it; ++previous) {
            if (previous->substr(previous->find_last_of('/') + 1) == basename) {
                return BindingResult::Failure({RuntimeErrorKind::DuplicateLibraryBasename, 0, basename});
            }
        }
    }
    return BindingResult::Success();
}

std::string JitRuntimeBindings::GetRuntimeDirectory(const std::string& runtimePath)
{
    const auto separator = runtimePath.find_last_of('/');
    return separator == std::string::npos ? "." : runtimePath.substr(0, separator);
}

BindingResult JitRuntimeBindings::LoadRuntime(const std::string& runtimePath)
{
    runtimeDirectory = GetRuntimeDirectory(runtimePath);
    runtime = dlopen(runtimePath.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!runtime) {
        const char* error = dlerror();
        return BindingResult::Failure(
            {RuntimeErrorKind::RuntimeOpenFailed, 0, error ? error : "unknown dynamic loader error"});
    }
    return BindingResult::Success();
}

BindingResult JitRuntimeBindings::LoadRequiredSymbols()
{
#define JIT_RUNTIME_FUNCTION(name, type, symbol, required) \
    if (required) { \
        dlerror(); \
        void* address = dlsym(runtime, symbol); \
        const char* error = dlerror(); \
        if (error || !address) { \
            return BindingResult::Failure({RuntimeErrorKind::RequiredSymbolMissing, 0, \
                std::string(symbol) + (error ? ": " : "") + (error ? error : "")}); \
        } \
        name = reinterpret_cast<type>(address); \
    }
#include "RuntimeFunctions.inc"
#undef JIT_RUNTIME_FUNCTION
    return BindingResult::Success();
}

BindingResult JitRuntimeBindings::InitializeRuntime(const ::RuntimeParam* params, bool owner)
{
    if (!owner) {
        return BindingResult::Success();
    }
    int result;
    if constexpr (Logger::DefaultConfig().runtime_log_mode == RuntimeLogMode::OFF) {
        auto quietParams = *params;
        quietParams.logParam.logLevel = RTLOG_OFF;
        result = InitRuntime(&quietParams);
    } else {
        result = InitRuntime(params);
    }
    if (result != 0) {
        return BindingResult::Failure({RuntimeErrorKind::InitializationFailed, result});
    }
    ownsRuntime = true;
    return BindingResult::Success();
}

BindingResult JitRuntimeBindings::LoadLibraries(const std::vector<std::string>& requestedLibraries)
{
    for (const auto& library : requestedLibraries) {
        if (auto result = LoadLibraryPath(library); !result) {
            return result;
        }
    }
    return BindingResult::Success();
}

BindingResult JitRuntimeBindings::LoadLibraryPath(const std::string& path)
{
    // Track the library before calling into the runtime so allocation failure
    // cannot lose a successfully loaded library.
    libraries.push_back(path);
    const int result = LoadLibrary(path.c_str());
    if (result != 0) {
        libraries.pop_back(); // the runtime rolls back a failed load
        return BindingResult::Failure({RuntimeErrorKind::LibraryLoadFailed, result, path});
    }
    return BindingResult::Success();
}

bool JitRuntimeBindings::IsLibraryLoaded(std::string_view name) const noexcept
{
    for (const auto& library : libraries) {
        const std::string_view path = library;
        if (path.substr(path.find_last_of('/') + 1) == name) {
            return true;
        }
    }
    return false;
}

std::optional<std::string> JitRuntimeBindings::FindPackageLibrary(const std::string& name) const
{
    if (auto path = FindLibraryInDirectories(librarySearchPaths, name)) {
        return path;
    }
    if (auto path = FindLibraryInDirectory(runtimeDirectory, name)) {
        return path;
    }
    if (auto path = FindLibraryInDirectories(importPaths, name)) {
        return path;
    }
    if (auto path = FindLibraryInDirectory(".", name)) {
        return path;
    }
    return FindLibraryInEnvironment(name);
}

BindingResult JitRuntimeBindings::EnsurePackage(std::string_view package)
{
    if (!initialized) {
        return BindingResult::Failure({RuntimeErrorKind::NotInitialized});
    }
    for (const auto& name : PackageLibraryNames(package)) {
        if (IsLibraryLoaded(name)) {
            return BindingResult::Success();
        }
        if (auto path = FindPackageLibrary(name)) {
            return LoadLibraryPath(*path);
        }
    }
    return BindingResult::Failure({RuntimeErrorKind::PackageLibraryNotFound, 0, std::string(package)});
}

SymbolResult JitRuntimeBindings::Resolve(SymbolName name)
{
    if (!initialized) {
        return SymbolResult::Failure({RuntimeErrorKind::NotInitialized});
    }
    if (name.empty() || name.find('\0') != SymbolName::npos) {
        return SymbolResult::Failure({RuntimeErrorKind::InvalidSymbolName});
    }
    std::string key(name);
    auto existing = symbols.find(key);
    if (existing != symbols.end()) {
        return SymbolResult::Success(existing->second);
    }
    auto find = FindSymbol;
    for (const auto& library : libraries) {
        if (void* address = find(library.c_str(), key.c_str())) {
            symbols.emplace(std::move(key), address);
            return SymbolResult::Success(address);
        }
    }
    dlerror();
    void* address = dlsym(runtime, key.c_str());
    const char* error = dlerror();
    if (error || !address) {
        return SymbolResult::Failure(
            {RuntimeErrorKind::SymbolNotFound, 0, key + (error ? std::string(": ") + error : "")});
    }
    symbols.emplace(std::move(key), address);
    return SymbolResult::Success(address);
}

#define JIT_RUNTIME_FUNCTION(name, type, symbol, required) \
    Result<type, RuntimeError> JitRuntimeBindings::Get##name() \
    { \
        if (!initialized) { \
            return Result<type, RuntimeError>::Failure({RuntimeErrorKind::NotInitialized}); \
        } \
        if (name) { \
            return Result<type, RuntimeError>::Success(name); \
        } \
        auto result = Resolve(symbol); \
        if (!result) { \
            return Result<type, RuntimeError>::Failure(std::move(result).Error()); \
        } \
        name = reinterpret_cast<type>(result.Value()); \
        return Result<type, RuntimeError>::Success(name); \
    }
#include "RuntimeFunctions.inc"
#undef JIT_RUNTIME_FUNCTION

const char* GetRuntimeErrorMessage(RuntimeErrorKind kind) noexcept
{
    switch (kind) {
        case RuntimeErrorKind::AlreadyInitialized: return "runtime bindings are already initialized or could not be shut down";
        case RuntimeErrorKind::MissingRuntimeConfig: return "a runtime library path and owner runtime parameters are required";
        case RuntimeErrorKind::EmptyLibraryPath: return "native library path is empty";
        case RuntimeErrorKind::EmptyLibraryBasename: return "native library basename is empty";
        case RuntimeErrorKind::DuplicateLibraryBasename: return "native library basenames must be distinct";
        case RuntimeErrorKind::RuntimeOpenFailed: return "cannot load Cangjie runtime";
        case RuntimeErrorKind::RequiredSymbolMissing: return "missing runtime symbol";
        case RuntimeErrorKind::InitializationFailed: return "InitCJRuntime failed";
        case RuntimeErrorKind::LibraryLoadFailed: return "LoadCJLibraryWithInit failed";
        case RuntimeErrorKind::PackageLibraryNotFound: return "package shared library not found";
        case RuntimeErrorKind::NotInitialized: return "runtime bindings are not initialized";
        case RuntimeErrorKind::InvalidSymbolName: return "invalid symbol name";
        case RuntimeErrorKind::SymbolNotFound: return "symbol not found";
        case RuntimeErrorKind::FinalizationSymbolMissing: return "runtime finalization symbol is missing";
        case RuntimeErrorKind::FinalizationFailed: return "runtime finalization failed";
        case RuntimeErrorKind::UnloadSymbolMissing: return "library unload symbol is missing";
        case RuntimeErrorKind::LibraryUnloadFailed: return "library unload failed";
        case RuntimeErrorKind::RuntimeCloseFailed: return "runtime library close failed";
        default: return "unknown runtime error";
    }
}

std::string FormatRuntimeError(const RuntimeError& error)
{
    auto format = [](RuntimeErrorKind kind, int code, const std::string& detail) {
        std::string message = GetRuntimeErrorMessage(kind);
        if (!detail.empty()) {
            message += ": " + detail;
        }
        if (code != 0) {
            message += " (" + std::to_string(code) + ")";
        }
        return message;
    };
    auto message = format(error.kind, error.code, error.detail);
    if (error.cleanup) {
        message += "; runtime cleanup failed: " +
            format(error.cleanup->kind, error.cleanup->code, error.cleanup->detail);
    }
    return message;
}

Result<void, RuntimeError> JitRuntimeBindings::Close()
{
    if (!runtime) {
        closeFailed = false;
        return Result<void, RuntimeError>::Success();
    }
    closeFailed = true;
    initialized = false;
    auto finalization = FinalizeRuntime();
    if (!finalization) {
        return finalization;
    }
    auto unload = UnloadLibraries();
    if (!unload) {
        return unload;
    }
    if (dlclose(runtime) != 0) {
        const char* error = dlerror();
        return Result<void, RuntimeError>::Failure(
            {RuntimeErrorKind::RuntimeCloseFailed, 0, error ? error : "unknown dynamic loader error"});
    }
    runtime = nullptr;
#define JIT_RUNTIME_FUNCTION(name, type, symbol, required) name = nullptr;
#include "RuntimeFunctions.inc"
#undef JIT_RUNTIME_FUNCTION
    symbols.clear();
    runtimeDirectory.clear();
    importPaths.clear();
    librarySearchPaths.clear();
    closeFailed = false;
    return Result<void, RuntimeError>::Success();
}

Result<void, RuntimeError> JitRuntimeBindings::FinalizeRuntime()
{
    if (!ownsRuntime) {
        return Result<void, RuntimeError>::Success();
    }
    if (!FiniRuntime) {
        return Result<void, RuntimeError>::Failure({RuntimeErrorKind::FinalizationSymbolMissing, 0, {}});
    }
    const auto code = FiniRuntime();
    if (code != 0) {
        return Result<void, RuntimeError>::Failure({RuntimeErrorKind::FinalizationFailed, code, {}});
    }
    ownsRuntime = false;
    return Result<void, RuntimeError>::Success();
}

Result<void, RuntimeError> JitRuntimeBindings::UnloadLibraries()
{
    // Keep library TypeInfos mapped until the owned runtime has stopped its collector.
    while (!libraries.empty()) {
        if (!UnloadLibrary) {
            return Result<void, RuntimeError>::Failure({RuntimeErrorKind::UnloadSymbolMissing, 0, libraries.back()});
        }
        const auto code = UnloadLibrary(libraries.back().c_str());
        if (code != 0) {
            return Result<void, RuntimeError>::Failure({RuntimeErrorKind::LibraryUnloadFailed, code, libraries.back()});
        }
        libraries.pop_back();
    }
    return Result<void, RuntimeError>::Success();
}
} // namespace Cangjie::JIT::X86_64
