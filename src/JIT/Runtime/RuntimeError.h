// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_RUNTIME_ERROR_H
#define CANGJIE_JIT_X86_64_RUNTIME_ERROR_H

#include <memory>
#include <optional>
#include <string>

#include "../X86_64/Types.h"

namespace Cangjie::JIT::X86_64 {
class JitRuntime;
enum class RuntimeErrorKind {
    MissingRuntimeConfig,
    EmptyLibraryPath,
    EmptyLibraryBasename,
    DuplicateLibraryBasename,
    RuntimeOpenFailed,
    RequiredSymbolMissing,
    InitializationFailed,
    LibraryLoadFailed,
    PackageLibraryNotFound,
    SymbolNotFound,
    FinalizationFailed,
    LibraryUnloadFailed,
    RuntimeCloseFailed,
    OutOfMemory,
};

struct RuntimeFailure {
    RuntimeErrorKind kind;
    int code = 0;
    std::string detail;
};

struct RuntimeError {
    RuntimeErrorKind kind;
    int code = 0;
    std::string detail;
    std::optional<RuntimeFailure> cleanup;
    // Present only when initialization rollback failed; the caller must close it explicitly.
    std::shared_ptr<JitRuntime> failedRuntime;
};

const char* GetRuntimeErrorMessage(RuntimeErrorKind kind) noexcept;
std::string FormatRuntimeError(const RuntimeError& error);

// Imported package, symbol and template lookups fail on the environment, not
// on a JIT bug; their failures cross the compilation boundary as exceptions.
inline void Require(const Result<void, RuntimeError>& result)
{
    if (!result) {
        throw result.Error();
    }
}

template<typename T>
T Require(const Result<T, RuntimeError>& result)
{
    if (!result) {
        throw result.Error();
    }
    return result.Value();
}
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_RUNTIME_ERROR_H
