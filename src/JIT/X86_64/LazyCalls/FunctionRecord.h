// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_LAZY_CALLS_FUNCTION_RECORD_H
#define CANGJIE_JIT_X86_64_LAZY_CALLS_FUNCTION_RECORD_H

#include <cstdint>
#include <string>
#include <variant>

namespace Cangjie::CHIR {
class Function;
}

namespace Cangjie::JIT::X86_64 {
using CodeAddress = uintptr_t;

enum class FunctionState : uint8_t {
    UNINITIALIZED,
    UNCOMPILED,
    COMPILING,
    READY,
    FAILED,
};

struct CompilationError {
    std::string message;
};
using CompilationResult = std::variant<CodeAddress, CompilationError>;

class FunctionRecord final {
public:
    explicit FunctionRecord(const CHIR::Function& function) noexcept;

    bool Initialize(CodeAddress entryStub, CodeAddress resolverThunk);
    const CHIR::Function& GetFunction() const noexcept;
    CodeAddress GetEntryStub() const;
    CodeAddress GetResolverThunk() const;
    const CodeAddress& GetTargetSlot() const noexcept;
    FunctionState GetState() const noexcept;
    const std::string& GetError() const noexcept;
    bool BeginCompilation();
    CompilationResult CompleteCompilation(CompilationResult result);
    void FailCompilation() noexcept;

private:
    const CHIR::Function& function;
    CodeAddress entryStub = 0;
    CodeAddress resolverThunk = 0;
    CodeAddress target = 0;
    FunctionState state = FunctionState::UNINITIALIZED;
    std::string error;
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_LAZY_CALLS_FUNCTION_RECORD_H
