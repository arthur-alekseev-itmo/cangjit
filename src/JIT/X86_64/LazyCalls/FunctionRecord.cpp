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

bool FunctionRecord::Initialize(CodeAddress newEntryStub, CodeAddress newResolverThunk)
{
    if (newEntryStub == 0 || newResolverThunk == 0) {
        return false;
    }
    if (state != FunctionState::UNINITIALIZED) {
        return entryStub == newEntryStub && resolverThunk == newResolverThunk;
    }
    entryStub = newEntryStub;
    resolverThunk = newResolverThunk;
    target = resolverThunk;
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

CodeAddress FunctionRecord::GetResolverThunk() const
{
    return resolverThunk;
}

const CodeAddress& FunctionRecord::GetTargetSlot() const noexcept
{
    return target;
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
        return CompilationError{"function is not being materialized"};
    }
    if (const auto* address = std::get_if<CodeAddress>(&result)) {
        if (*address != 0 && *address != entryStub && *address != resolverThunk) {
            target = *address;
            state = FunctionState::READY;
            return result;
        }
        result = CompilationError{"function materializer returned an invalid target"};
    }
    error.swap(std::get<CompilationError>(result).message);
    state = FunctionState::FAILED;
    return CompilationError{error};
}

void FunctionRecord::FailCompilation() noexcept
{
    if (state == FunctionState::COMPILING) {
        state = FunctionState::FAILED;
    }
}
} // namespace Cangjie::JIT::X86_64
