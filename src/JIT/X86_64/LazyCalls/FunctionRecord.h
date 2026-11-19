// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_LAZY_CALLS_FUNCTION_RECORD_H
#define CANGJIE_JIT_X86_64_LAZY_CALLS_FUNCTION_RECORD_H

#include <cstdint>
#include <string>

#include "../Types.h"

namespace Cangjie::CHIR {
class Function;
}

namespace Cangjie::JIT::X86_64 {

enum class FunctionState : uint8_t {
    UNINITIALIZED,
    UNCOMPILED,
    COMPILING,
    READY,
    FAILED,
};


class FunctionRecord final {
public:
    explicit FunctionRecord(const CHIR::Function& function) noexcept;

    bool Initialize(CodeAddress entryStub);
    const CHIR::Function& GetFunction() const noexcept;
    CodeAddress GetEntryStub() const;
    CodeAddress GetCompiledTarget() const noexcept;
    FunctionState GetState() const noexcept;
    const std::string& GetError() const noexcept;
    bool BeginCompilation();
    CompilationResult CompleteCompilation(CompilationResult result);
    void FailCompilation() noexcept;

private:
    const CHIR::Function& function;
    CodeAddress entryStub = 0;
    CodeAddress compiledTarget = 0;
    FunctionState state = FunctionState::UNINITIALIZED;
    std::string error;
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_LAZY_CALLS_FUNCTION_RECORD_H
