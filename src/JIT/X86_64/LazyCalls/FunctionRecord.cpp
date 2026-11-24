// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "FunctionRecord.h"

namespace Cangjie::JIT::X86_64 {
FunctionRecord::FunctionRecord(const CHIR::Function& function) noexcept : function(function)
{
}

bool FunctionRecord::Initialize(CodeAddress newEntryStub)
{
    if (newEntryStub == 0) {
        return false;
    }
    if (state != FunctionState::UNINITIALIZED) {
        return entryStub == newEntryStub;
    }
    entryStub = newEntryStub;
    state = FunctionState::UNCOMPILED;
    return true;
}

const CHIR::Function& FunctionRecord::GetFunction() const noexcept
{
    return function;
}

CodeAddress FunctionRecord::GetEntryStub() const
{
    return entryStub;
}


CodeAddress FunctionRecord::GetCompiledTarget() const noexcept
{
    return compiledTarget;
}


FunctionState FunctionRecord::GetState() const noexcept
{
    return state;
}

const std::string& FunctionRecord::GetError() const noexcept
{
    return error;
}

bool FunctionRecord::BeginCompilation()
{
    if (state != FunctionState::UNCOMPILED) {
        return false;
    }
    error = "function materialization failed";
    state = FunctionState::COMPILING;
    return true;
}

CompilationResult FunctionRecord::CompleteCompilation(CompilationResult result)
{
    if (state != FunctionState::COMPILING) {
        return CompilationResult::Failure({"function is not being materialized"});
    }

    if (!result) {
        error.swap(result.Error().message);
        state = FunctionState::FAILED;
        return CompilationResult::Failure({error});
    }

    const auto address = result.Value();
    if (address != 0 && address != entryStub) {
        compiledTarget = address;
        state = FunctionState::READY;
        return result;
    }
    result = CompilationResult::Failure({"function materializer returned an invalid target"});
    return result;
}

void FunctionRecord::FailCompilation() noexcept
{
    state = FunctionState::FAILED;
}
} // namespace Cangjie::JIT::X86_64
