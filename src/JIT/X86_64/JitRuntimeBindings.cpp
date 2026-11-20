// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "JitRuntimeBindings.h"

#include <cstdio>
#include <dlfcn.h>
#include <utility>

namespace Cangjie::JIT::X86_64 {
namespace {
// These signatures follow the public Cangjie.h API. RuntimeParam is opaque
// here so building the compiler does not require an installed runtime SDK.
using InitRuntime = int (*)(const ::RuntimeParam*);
using FiniRuntime = int (*)();
using LibraryOperation = int (*)(const char*);
using FindSymbol = void* (*)(const char*, const char*);

constexpr const char* REQUIRED_SYMBOLS[] = {
    "InitCJRuntime", "FiniCJRuntime", "LoadCJLibraryWithInit",
    "UnloadCJLibrary", "FindCJSymbol", "RunCJTask", "GetTaskRet", "ReleaseHandle", "CJ_MCC_N2CStub",
    "CJ_MCC_ThrowException", "CJ_MCC_GetExceptionWrapper", "CJ_MCC_PostThrowException"
};
}

BindingResult JitRuntimeBindings::Initialize(const RuntimeConfig& config)
{
    if (runtime != nullptr) {
        return "runtime bindings are already initialized or could not be shut down";
    }
    const auto& [runtimePath, requestedLibraries, params, owner, stackGrowth] = config;
    // This is a host assertion, not a runtime switch. Consumers must pass the
    // same value to the metadata encoder; no public query exposes the mode.
    (void)stackGrowth;
    if (runtimePath.empty() || (owner && params == nullptr)) {
        return "a runtime library path and owner runtime parameters are required";
    }
    for (auto it = requestedLibraries.begin(); it != requestedLibraries.end(); ++it) {
        if (it->empty()) {
            return "native library path is empty";
        }
        const auto basename = it->substr(it->find_last_of('/') + 1);
        if (basename.empty()) {
            return "native library basename is empty";
        }
        for (auto previous = requestedLibraries.begin(); previous != it; ++previous) {
            if (previous->substr(previous->find_last_of('/') + 1) == basename) {
                return "native library basenames must be distinct: " + basename;
            }
        }
    }
    const auto separator = runtimePath.find_last_of('/');
    runtimeDirectory = separator == std::string::npos ? "." : runtimePath.substr(0, separator);

    runtime = dlopen(runtimePath.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!runtime) {
        const char* error = dlerror();
        return std::string("cannot load Cangjie runtime: ") + (error ? error : "unknown dynamic loader error");
    }

    try {
        for (const char* name : REQUIRED_SYMBOLS) {
            dlerror();
            void* address = dlsym(runtime, name);
            const char* error = dlerror();
            if (error || !address) {
                std::string message = std::string("missing runtime symbol ") + name + (error ? ": " : "") +
                    (error ? error : "");
                Shutdown();
                return message;
            }
            symbols.emplace(name, address);
        }
        libraries.reserve(requestedLibraries.size());
        if (owner) {
            int result = reinterpret_cast<InitRuntime>(symbols.at("InitCJRuntime"))(params);
            if (result != 0) {
                Shutdown();
                return "InitCJRuntime failed with error " + std::to_string(result);
            }
            ownsRuntime = true;
        }
        for (const auto& library : requestedLibraries) {
            // Copy before calling into the runtime, so an allocation failure
            // cannot lose track of a successfully loaded library.
            libraries.push_back(library);
            int result = reinterpret_cast<LibraryOperation>(symbols.at("LoadCJLibraryWithInit"))(library.c_str());
            if (result != 0) {
                libraries.pop_back(); // the runtime rolls back a failed load
                Shutdown();
                return "LoadCJLibraryWithInit failed for " + library + ": " + std::to_string(result);
            }
        }
        initialized = true;
        return {};
    } catch (...) {
        Shutdown();
        throw;
    }
}

BindingResult JitRuntimeBindings::EnsureStandardPackage(std::string_view package)
{
    constexpr std::string_view prefix = "std.";
    if (package.substr(0, prefix.size()) != prefix) {
        return {};
    }
    if (!initialized) {
        return "runtime bindings are not initialized";
    }
    const auto basename = std::string("libcangjie-std-") + std::string(package.substr(prefix.size())) + ".so";
    for (const auto& library : libraries) {
        if (library.substr(library.find_last_of('/') + 1) == basename) {
            return {};
        }
    }
    const auto path = runtimeDirectory + "/" + basename;
    libraries.push_back(path);
    const int result = reinterpret_cast<LibraryOperation>(symbols.at("LoadCJLibraryWithInit"))(path.c_str());
    if (result != 0) {
        libraries.pop_back();
        return "LoadCJLibraryWithInit failed for " + path + ": " + std::to_string(result);
    }
    return {};
}

SymbolResult JitRuntimeBindings::Resolve(SymbolName name)
{
    if (!initialized) {
        return std::string("runtime bindings are not initialized");
    }
    if (name.empty() || name.find('\0') != SymbolName::npos) {
        return std::string("invalid symbol name");
    }
    std::string key(name);
    auto existing = symbols.find(key);
    if (existing != symbols.end()) {
        return existing->second;
    }
    auto find = reinterpret_cast<FindSymbol>(symbols.at("FindCJSymbol"));
    for (const auto& library : libraries) {
        if (void* address = find(library.c_str(), key.c_str())) {
            symbols.emplace(std::move(key), address);
            return address;
        }
    }
    dlerror();
    void* address = dlsym(runtime, key.c_str());
    const char* error = dlerror();
    if (error || !address) {
        return std::string("symbol not found: ") + key + (error ? std::string(": ") + error : "");
    }
    symbols.emplace(std::move(key), address);
    return address;
}

void JitRuntimeBindings::Shutdown()
{
    if (!runtime) {
        return;
    }
    initialized = false;
    auto unload = symbols.find("UnloadCJLibrary");
    while (!libraries.empty()) {
        if (unload == symbols.end() || reinterpret_cast<LibraryOperation>(unload->second)(libraries.back().c_str()) != 0) {
            std::fprintf(stderr, "JIT runtime bindings: failed to unload %s\n", libraries.back().c_str());
            return; // Retain the runtime and remaining handles rather than invalidate live code.
        }
        libraries.pop_back();
    }
    if (ownsRuntime) {
        auto fini = symbols.find("FiniCJRuntime");
        if (fini == symbols.end() || reinterpret_cast<FiniRuntime>(fini->second)() != 0) {
            std::fprintf(stderr, "JIT runtime bindings: failed to finish Cangjie runtime\n");
            return; // Do not unload a runtime that could still be active.
        }
        ownsRuntime = false;
    }
    symbols.clear();
    if (dlclose(runtime) == 0) {
        runtime = nullptr;
    } else {
        const char* error = dlerror();
        std::fprintf(stderr, "JIT runtime bindings: failed to close runtime: %s\n", error ? error : "unknown error");
    }
}
} // namespace Cangjie::JIT::X86_64
