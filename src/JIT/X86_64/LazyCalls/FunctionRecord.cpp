// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "FunctionRecord.h"

#include "../JitAssert.h"

namespace Cangjie::JIT::X86_64 {
FunctionRecord::FunctionRecord(const CHIR::Function& function) noexcept : function(function)
{
}

void FunctionRecord::Initialize(CodeAddress newEntryStub)
{
    JIT_ASSERT(newEntryStub != 0, "resolver stub allocation is required");
    JIT_ASSERT(entryStub == 0 && compiledTarget == 0 && state == FunctionState::UNCOMPILED,
        "function record is initialized once");
    entryStub = newEntryStub;
}

const CHIR::Function& FunctionRecord::GetFunction() const noexcept
{
    return function;
}

CodeAddress FunctionRecord::GetEntryStub() const noexcept
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

void FunctionRecord::BeginCompilation()
{
    JIT_ASSERT(entryStub != 0 && compiledTarget == 0 && state == FunctionState::UNCOMPILED,
        "materialization starts from an initialized uncompiled record");
    state = FunctionState::COMPILING;
}

CodeAddress FunctionRecord::CompleteCompilation(CodeAddress target)
{
    JIT_ASSERT(state == FunctionState::COMPILING, "function is not being materialized");
    JIT_ASSERT(target != 0 && target != entryStub, "function materializer returned an invalid target");
    compiledTarget = target;
    state = FunctionState::READY;
    return target;
}
} // namespace Cangjie::JIT::X86_64
