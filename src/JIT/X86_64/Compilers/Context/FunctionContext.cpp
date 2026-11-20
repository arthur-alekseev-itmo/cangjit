// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "FunctionContext.h"

#include "GlobalContext.h"
#include "../../RegisterAllocation/SafepointLiveness.h"
#include "../../Utils/StackFrameUtils.h"

#include <cassert>
#include <limits>
#include <ostream>
#include <utility>

#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/Mangle/CHIRTypeManglingUtils.h"
#include "cangjie/CHIR/IR/Value/Value.h"

namespace Cangjie::JIT::X86_64 {

FunctionContext::FunctionContext(GlobalContext& globalContext, const CHIR::Function* function,
    ValueNumeration valueNumeration, InterferenceGraph interferenceGraph, RegisterAllocation registerAllocation) noexcept
    : globalContext(globalContext),
      error(asmjit::kErrorOk),
      metadataBuilder(globalContext.IsStackGrowthEnabled()),
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
JitMetadataBuilder& FunctionContext::GetMetadataBuilder() noexcept
{
    return metadataBuilder;
}
bool FunctionContext::RecordSafepoint(const CHIR::Expression& expression)
{
    const auto offset = assembler.offset();
    if (offset < 4 || offset - 4 > std::numeric_limits<uint32_t>::max()) {
        Fail(JitFailureKind::UnsupportedExpression, expression.GetExprKind());
        return false;
    }
    auto result = ComputeSafepointLiveness(*function, valueNumeration, registerAllocation, expression);
    auto* roots = std::get_if<SafepointRoots>(&result);
    if (!roots) {
        Fail(JitFailureKind::UnsupportedExpression, expression.GetExprKind());
        return false;
    }
    auto& [gc, stack, derived] = *roots;
    if (!globalContext.IsStackGrowthEnabled()) {
        stack = {};
    }
    metadataBuilder.RecordSafepoint(static_cast<uint32_t>(offset - 4), gc.first, std::move(gc.second),
        stack.first, std::move(stack.second), 0, std::move(derived));
    return true;
}
bool FunctionContext::EmitExceptionLanding(const CHIR::Block& block, size_t callStart, size_t callEnd)
{
    const auto landingOffset = assembler.offset();
    if (!block.IsLandingPadBlock() || callStart < 4 || callEnd <= callStart ||
        callEnd - 4 > std::numeric_limits<uint32_t>::max() ||
        landingOffset < 4 || landingOffset - 4 > std::numeric_limits<uint32_t>::max()) {
        Fail(JitFailureKind::UnsupportedExpression);
        return false;
    }
    std::vector<uintptr_t> types;
    const auto exceptions = block.GetExceptions();
    if (exceptions.empty()) {
        types.push_back(0);
    } else {
        auto symbol = globalContext.GetRuntimeBindings().Resolve("CJ_MCC_GetTypeByMangledName");
        const auto* address = std::get_if<void*>(&symbol);
        if (!address || !*address) {
            Fail(JitFailureKind::UnresolvedSymbol);
            return false;
        }
        using LookupType = void* (*)(const char*);
        const auto lookup = reinterpret_cast<LookupType>(*address);
        types.reserve(exceptions.size());
        for (const auto* type : exceptions) {
            if (!type) {
                Fail(JitFailureKind::UnsupportedExpression);
                return false;
            }
            const auto name = CHIR::GetTypeQualifiedName(*type, true);
            auto* info = lookup(name.c_str());
            if (!info) {
                Fail(JitFailureKind::UnresolvedSymbol);
                return false;
            }
            types.push_back(reinterpret_cast<uintptr_t>(info));
        }
    }
    const auto slots = registerAllocation.GetStackSlotCount();
    if (slots == std::numeric_limits<size_t>::max()) {
        Fail(JitFailureKind::StackFrameTooLarge);
        return false;
    }
    const auto frameSize = StackFrameUtils::GetFrameSize(slots + 1);
    if (!frameSize) {
        Fail(JitFailureKind::StackFrameTooLarge);
        return false;
    }
    const auto landing = metadataBuilder.RecordLandingPad(static_cast<uint32_t>(landingOffset - 4), std::move(types));
    metadataBuilder.RecordCallSite(static_cast<uint32_t>(callStart - 4),
        static_cast<uint32_t>(callEnd - callStart), landing);
    assembler.lea(asmjit::x86::rsp, asmjit::x86::ptr(asmjit::x86::rbp, -static_cast<int32_t>(*frameSize)));
    assembler.jmp(GetBlockLabel(&block));
    return GetError() == asmjit::kErrorOk;
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

std::vector<const CHIR::Function*>& FunctionContext::GetFunctionsToStub() noexcept
{
    return functionsToStub;
}

const std::vector<const CHIR::Function*>& FunctionContext::GetFunctionsToStub() const noexcept
{
    return functionsToStub;
}
std::vector<PendingLazyCallSite>& FunctionContext::GetLazyCallSites() noexcept
{
    return lazyCallSites;
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
