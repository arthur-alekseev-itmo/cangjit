// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_LOGGER_H
#define CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_LOGGER_H

#include <asmjit/core.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>


namespace Cangjie::JIT::X86_64 {
class Liveness;
class RegisterAllocation;
class ValueNumeration;

enum class FuncPrintMode : uint8_t {
    NONE = 0,
    ASM = 1 << 0,
    ENTRY = 1 << 1,
    REGALLOC = 1 << 2,
    LIVENESS = 1 << 3,
    ALL = 0b1111,
};

enum class RuntimeLogMode : uint8_t { OFF, ON };

struct DebugConfig {
    FuncPrintMode func_print_mode;
    RuntimeLogMode runtime_log_mode;
};

class Logger final {
public:
    static constexpr DebugConfig DefaultConfig() noexcept
    {
        return {FuncPrintMode::ASM, RuntimeLogMode::ON};
    }

    Logger() noexcept;

    void AttachAssemblyLogger(asmjit::CodeHolder& code, std::string_view name);
    void LogFunctionCompilationBegin(std::string_view name) const;
    void LogFunctionCompilationEnd(std::string_view name, bool succeeded, std::string_view error) const;
    void LogRegisterAllocation(
        std::string_view name, const ValueNumeration& numeration, const RegisterAllocation& allocation) const;
    void LogResolverStubEmission(asmjit::CodeHolder& code, std::string_view name);
    void LogLiveness(std::string_view name, const Liveness& liveness, const ValueNumeration& numeration) const;
    void LogFunctionCreated(std::string_view name, uintptr_t entry, size_t codeSize, size_t frameSize,
        uintptr_t metadata, size_t metadataSize) const;
    void LogResolverStubCreated(std::string_view name, uintptr_t entry) const;
    void LogCallPatched(std::string_view name, uintptr_t caller, uintptr_t immediate, uintptr_t target) const;

private:
    constexpr bool IsEnabled(FuncPrintMode mode) const noexcept
    {
        return (static_cast<uint8_t>(debugConfig.func_print_mode) & static_cast<uint8_t>(mode)) != 0;
    }

    DebugConfig debugConfig{DefaultConfig()};
    asmjit::FileLogger assemblyLogger{stderr};
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_LOGGER_H
