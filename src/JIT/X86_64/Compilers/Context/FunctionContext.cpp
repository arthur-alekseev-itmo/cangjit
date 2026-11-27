// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "FunctionContext.h"

#include "GlobalContext.h"
#include "../../JitAssert.h"
#include "../../Utils/StackFrameUtils.h"

#include <utility>

#include "cangjie/CHIR/IR/Value/Value.h"

namespace Cangjie::JIT::X86_64 {
FunctionContext::FunctionContext(GlobalContext& globalContext, const CHIR::Function* function,
    ValueNumeration valueNumeration, Liveness liveness, RegisterAllocation registerAllocation) noexcept
    : globalContext(globalContext),
      error(asmjit::kErrorOk),
      metadata(globalContext.IsStackGrowthEnabled()),
      function(function),
      valueNumeration(std::move(valueNumeration)),
      liveness(std::move(liveness)),
      registerAllocation(std::move(registerAllocation))
{
}

void FunctionContext::Prepare()
{
    ReserveHiddenTypeInfoSlots();
    InitializeAssembler();
}

void FunctionContext::ReserveHiddenTypeInfoSlots() noexcept
{
    const auto genericCount = function->GetGenericTypeParams().size();
    hiddenTypeInfoCount = genericCount + (function->GetParentCustomTypeDef() ? 1U : 0U) +
        (function->TestAttr(CHIR::Attribute::STATIC) ? 1U : 0U);
    hiddenTypeInfoBase = registerAllocation.ReserveStackSlots(hiddenTypeInfoCount);
}

void FunctionContext::InitializeAssembler()
{
    error = code.init(globalContext.GetRuntime().environment());
    JIT_ASSERT(error == asmjit::kErrorOk, "cannot initialize code holder");
    globalContext.GetLogger().AttachAssemblyLogger(code, function->GetIdentifier());
    code.setErrorHandler(&emitterErrorHandler);
    error = code.attach(&assembler);
    JIT_ASSERT(error == asmjit::kErrorOk, "cannot attach assembler");
    epilogueLabel = assembler.newLabel();
    JIT_ASSERT(epilogueLabel.isValid(), "failed to create epilogue label");
}

GlobalContext& FunctionContext::GetGlobalContext() const noexcept
{
    return globalContext;
}

asmjit::CodeHolder& FunctionContext::GetCode() noexcept
{
    return code;
}

asmjit::x86::Assembler& FunctionContext::GetAssembler() noexcept
{
    return assembler;
}
FunctionMetadata& FunctionContext::GetMetadata() noexcept
{
    return metadata;
}

int32_t FunctionContext::GetHiddenTypeInfoOffset(size_t index) const noexcept
{
    JIT_ASSERT(index < hiddenTypeInfoCount, "hidden type information slot is out of range");
    return StackFrameUtils::GetSlotOffset(hiddenTypeInfoBase + index, registerAllocation.GetStackSlotCount());
}

asmjit::Error FunctionContext::GetError() const noexcept
{
    return error == asmjit::kErrorOk ? emitterErrorHandler.firstError : error;
}

asmjit::Label FunctionContext::GetBlockLabel(const CHIR::Block* block)
{
    JIT_ASSERT(block != nullptr, "block destination is required");
    auto found = blockLabels.find(block);
    if (found != blockLabels.end()) {
        return found->second;
    }
    auto label = assembler.newLabel();
    JIT_ASSERT(label.isValid(), "failed to create block label");
    blockLabels.emplace(block, label);
    return label;
}

asmjit::Label FunctionContext::GetEpilogueLabel() const noexcept
{
    return epilogueLabel;
}

const CHIR::Function* FunctionContext::GetFunction() const noexcept
{
    return function;
}

std::vector<PendingLazyCallSite>& FunctionContext::GetLazyCallSites() noexcept
{
    return lazyCallSites;
}


const ValueNumeration& FunctionContext::GetValueNumeration() const noexcept
{
    return valueNumeration;
}


const RegisterAllocation& FunctionContext::GetRegisterAllocation() const noexcept
{
    return registerAllocation;
}

const Location* FunctionContext::GetValueLocation(const CHIR::Value* value) noexcept
{
    const auto id = valueNumeration.RequireId(value);
    return &registerAllocation.GetLocation(id);
}


} // namespace Cangjie::JIT::X86_64
