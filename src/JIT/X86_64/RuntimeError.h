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

namespace Cangjie::JIT::X86_64 {
class JitRuntime;
enum class RuntimeErrorKind {
    AlreadyInitialized,
    MissingRuntimeConfig,
    EmptyLibraryPath,
    EmptyLibraryBasename,
    DuplicateLibraryBasename,
    RuntimeOpenFailed,
    RequiredSymbolMissing,
    InitializationFailed,
    LibraryLoadFailed,
    PackageLibraryNotFound,
    NotInitialized,
    InvalidSymbolName,
    SymbolNotFound,
    FinalizationSymbolMissing,
    FinalizationFailed,
    UnloadSymbolMissing,
    LibraryUnloadFailed,
    RuntimeCloseFailed,
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
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_RUNTIME_ERROR_H
