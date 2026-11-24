// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "FunctionContext.h"

#include "GlobalContext.h"
#include "../../Utils/StackFrameUtils.h"

#include <cassert>
#include <utility>

#include "cangjie/CHIR/IR/Value/Value.h"

namespace Cangjie::JIT::X86_64 {
FunctionContext::FunctionContext(GlobalContext& globalContext, const CHIR::Function* function,
    ValueNumeration valueNumeration, Liveness liveness, RegisterAllocation registerAllocation) noexcept
    : globalContext(globalContext),
      error(asmjit::kErrorOk),
      metadataBuilder(globalContext.IsStackGrowthEnabled()),
      function(function),
      valueNumeration(std::move(valueNumeration)),
      liveness(std::move(liveness)),
      registerAllocation(std::move(registerAllocation))
{
    assert(function != nullptr);
}

bool FunctionContext::Prepare()
{
    ReserveHiddenTypeInfoSlots();
    return InitializeAssembler();
}

void FunctionContext::ReserveHiddenTypeInfoSlots() noexcept
{
    const auto genericCount = function->GetGenericTypeParams().size();
    hiddenTypeInfoCount = genericCount + (function->GetParentCustomTypeDef() ? 1U : 0U) +
        (function->TestAttr(CHIR::Attribute::STATIC) ? 1U : 0U);
    hiddenTypeInfoBase = registerAllocation.ReserveStackSlots(hiddenTypeInfoCount);
}

bool FunctionContext::InitializeAssembler()
{
    error = code.init(globalContext.GetRuntime().environment());
    if (error != asmjit::kErrorOk) {
        return false;
    }
    if (auto* logger = globalContext.GetLogger()) {
        logger->logf("[JIT] assembly function %s\n", function->GetIdentifier().c_str());
        code.setLogger(logger);
    }
    code.setErrorHandler(&emitterErrorHandler);
    error = code.attach(&assembler);
    if (error != asmjit::kErrorOk) {
        return false;
    }
    epilogueLabel = assembler.newLabel();
    if (!epilogueLabel.isValid()) {
        Fail(JitFailureKind::EpilogueLabelCreationFailed);
        return false;
    }
    return true;
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
JitMetadataBuilder& FunctionContext::GetMetadataBuilder() noexcept
{
    return metadataBuilder;
}

std::optional<int32_t> FunctionContext::GetHiddenTypeInfoOffset(size_t index) const noexcept
{
    if (index >= hiddenTypeInfoCount) {
        return std::nullopt;
    }
    return StackFrameUtils::GetSlotOffset(hiddenTypeInfoBase + index, registerAllocation.GetStackSlotCount());
}

asmjit::Error FunctionContext::GetError() const noexcept
{
    return error == asmjit::kErrorOk ? emitterErrorHandler.firstError : error;
}

asmjit::Label FunctionContext::GetBlockLabel(const CHIR::Block* block)
{
    if (!block) {
        Fail(JitFailureKind::MissingBranchDestination);
        return {};
    }
    auto found = blockLabels.find(block);
    if (found != blockLabels.end()) {
        return found->second;
    }
    auto label = assembler.newLabel();
    if (!label.isValid()) {
        Fail(JitFailureKind::BlockLabelCreationFailed);
        return label;
    }
    blockLabels.emplace(block, label);
    return label;
}

asmjit::Label FunctionContext::GetEpilogueLabel() const noexcept
{
    return epilogueLabel;
}

void FunctionContext::Fail(JitFailureKind kind, std::optional<CHIR::ExprKind> expressionKind) noexcept
{
    if (!failure) {
        failure = JitFailure{kind, expressionKind};
    }
}

const std::optional<JitFailure>& FunctionContext::GetFailure() const noexcept
{
    return failure;
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


} // namespace Cangjie::JIT::X86_64
