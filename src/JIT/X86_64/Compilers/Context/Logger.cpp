// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Logger.h"

#include <cstdio>
#include <iostream>

#include "../../RegisterAllocation/Liveness.h"
#include "../../RegisterAllocation/RegisterAllocation.h"
#include "../../RegisterAllocation/ValueNumeration.h"

namespace Cangjie::JIT::X86_64 {
Logger::Logger() noexcept
{
    assemblyLogger.addFlags(asmjit::FormatFlags::kMachineCode | asmjit::FormatFlags::kHexImms);
}

void Logger::LogResolverStubEmission(asmjit::CodeHolder& code, std::string_view name)
{
    if (!IsEnabled(FuncPrintMode::ASM)) {
        return;
    }
    assemblyLogger.logf("[JIT] emitting stub for %.*s\n", static_cast<int>(name.size()), name.data());
    code.setLogger(&assemblyLogger);
}

void Logger::AttachAssemblyLogger(asmjit::CodeHolder& code, std::string_view name)
{
    if (!IsEnabled(FuncPrintMode::ASM)) {
        return;
    }
    assemblyLogger.logf("[JIT] assembly function %.*s\n", static_cast<int>(name.size()), name.data());
    code.setLogger(&assemblyLogger);
}

void Logger::LogFunctionCompilationBegin(std::string_view name) const
{
    if (IsEnabled(FuncPrintMode::ENTRY)) {
        std::fprintf(stderr, "[JIT] compiling function %.*s\n", static_cast<int>(name.size()), name.data());
    }
}

void Logger::LogFunctionCompilationEnd(std::string_view name, bool succeeded, std::string_view error) const
{
    if (!IsEnabled(FuncPrintMode::ENTRY)) {
        return;
    }
    if (succeeded) {
        std::fprintf(stderr, "[JIT] finished function %.*s\n", static_cast<int>(name.size()), name.data());
    } else {
        std::fprintf(stderr, "[JIT] failed function %.*s: %.*s\n", static_cast<int>(name.size()), name.data(),
            static_cast<int>(error.size()), error.data());
    }
}

void Logger::LogRegisterAllocation(
    std::string_view name, const ValueNumeration& numeration, const RegisterAllocation& allocation) const
{
    if (!IsEnabled(FuncPrintMode::REGALLOC)) {
        return;
    }
    std::cerr << "[JIT] register allocation " << name << '\n';
    numeration.Dump(std::cerr);
    allocation.Dump(std::cerr);
}

void Logger::LogLiveness(std::string_view name, const Liveness& liveness, const ValueNumeration& numeration) const
{
    if (!IsEnabled(FuncPrintMode::LIVENESS)) {
        return;
    }
    std::cerr << "[JIT] liveness " << name << '\n';
    liveness.Dump(std::cerr, numeration);
}

void Logger::LogFunctionCreated(std::string_view name, uintptr_t entry, size_t codeSize, size_t frameSize,
    uintptr_t metadata, size_t metadataSize) const
{
    if (IsEnabled(FuncPrintMode::ENTRY)) {
        std::fprintf(stderr, "[JIT] function created name=%.*s entry=%p code_size=%zu frame_size=%zu "
                             "metadata=%p metadata_size=%zu\n",
            static_cast<int>(name.size()), name.data(), reinterpret_cast<void*>(entry), codeSize, frameSize,
            reinterpret_cast<void*>(metadata), metadataSize);
    }
}

void Logger::LogResolverStubCreated(std::string_view name, uintptr_t entry) const
{
    if (IsEnabled(FuncPrintMode::ENTRY)) {
        std::fprintf(stderr, "[JIT] stub created name=%.*s entry=%p\n", static_cast<int>(name.size()), name.data(),
            reinterpret_cast<void*>(entry));
    }
}

void Logger::LogCallPatched(std::string_view name, uintptr_t caller, uintptr_t immediate, uintptr_t target) const
{
    if (IsEnabled(FuncPrintMode::ENTRY)) {
        std::fprintf(stderr, "[JIT] call patched name=%.*s caller=%p immediate=%p target=%p\n",
            static_cast<int>(name.size()), name.data(), reinterpret_cast<void*>(caller),
            reinterpret_cast<void*>(immediate), reinterpret_cast<void*>(target));
    }
}
} // namespace Cangjie::JIT::X86_64
