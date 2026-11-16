// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "FunctionContext.h"

#include "GlobalContext.h"

#include <cassert>
#include <ostream>
#include <utility>

#include "cangjie/CHIR/IR/Value/Value.h"

namespace Cangjie::JIT::X86_64 {

FunctionContext::FunctionContext(GlobalContext& globalContext, const CHIR::Function* function,
    ValueNumeration valueNumeration, InterferenceGraph interferenceGraph, RegisterAllocation registerAllocation) noexcept
    : globalContext(globalContext),
      error(asmjit::kErrorOk),
      function(function),
      valueNumeration(std::move(valueNumeration)),
      interferenceGraph(std::move(interferenceGraph)),
      registerAllocation(std::move(registerAllocation))
{
    assert(function != nullptr);
    error = code.init(globalContext.GetRuntime().environment());
    if (error == asmjit::kErrorOk) {
        code.setErrorHandler(&emitterErrorHandler);
        error = code.attach(&assembler);
        if (error == asmjit::kErrorOk) {
            epilogueLabel = assembler.newLabel();
            if (!epilogueLabel.isValid()) {
                Fail(JitFailureKind::EpilogueLabelCreationFailed);
            }
        }
    }
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

const ValueNumeration& FunctionContext::GetValueNumeration() const noexcept
{
    return valueNumeration;
}

const InterferenceGraph& FunctionContext::GetInterferenceGraph() const noexcept
{
    return interferenceGraph;
}

const RegisterAllocation& FunctionContext::GetRegisterAllocation() const noexcept
{
    return registerAllocation;
}

void FunctionContext::Dump(std::ostream& output) const
{
    output << "FunctionContext(function=" << function->GetIdentifier() << ")\n";
    output << "Body:\n" << function->GetBody()->ToString(0) << '\n';
    valueNumeration.Dump(output);
    interferenceGraph.Dump(output);
    registerAllocation.Dump(output);
}

} // namespace Cangjie::JIT::X86_64
