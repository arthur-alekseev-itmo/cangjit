// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "LocalFinalizers.h"

#include <algorithm>
#include <iterator>

#include "FunctionContext.h"
#include "GlobalContext.h"
#include "../../RegisterAllocation/LocalFinalizerFrontier.h"
#include "../../Utils/MemoryUtils.h"
#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Expression/Terminator.h"
#include "cangjie/CHIR/IR/Type/ClassDef.h"
#include "cangjie/CHIR/IR/Value/Value.h"
#include "cangjie/Utils/CheckUtils.h"

namespace Cangjie::JIT::X86_64 {
bool LocalFinalizers::Prepare()
{
    const auto* function = context.GetFunction();
    const auto* body = function->GetBody();
    if (!body) {
        context.Fail(JitFailureKind::InvalidFunctionContext);
        return false;
    }
    for (const auto* block : body->GetAllBlocks()) {
        if (block->GetFuncOrLambdaBody() != body) {
            continue;
        }
        for (size_t i = 0; i < block->GetExpressionsNum(); ++i) {
            if (!PrepareAllocation(*block->GetExpressionByIdx(i))) {
                return false;
            }
        }
    }
    return true;
}

bool LocalFinalizers::PrepareAllocation(const CHIR::Expression& expression)
{
    if (expression.GetExprKind() != CHIR::ExprKind::ALLOCATE &&
        expression.GetExprKind() != CHIR::ExprKind::TRY_ALLOCATE) {
        return true;
    }
    const auto& allocation = static_cast<const CHIR::AllocateBase&>(expression);
    const auto* type = allocation.GetType();
    if (!type || !type->IsClass() || !type->IsLocalRegion()) {
        return true;
    }
    const auto* def = static_cast<const CHIR::ClassType*>(type)->GetClassDef();
    if (!def || !def->GetFinalizer()) {
        return true;
    }
    auto frontier = ComputeLocalFinalizerFrontier(
        *context.GetFunction(), allocation, context.GetLiveness(), context.GetValueNumeration());
    if (!frontier || !allocation.GetResult()) {
        context.Fail(JitFailureKind::UnsupportedLocalFinalizer, expression.GetExprKind());
        return false;
    }
    for (const auto& site : frontier.Value()) {
        const auto* after = std::get<0>(site);
        CJC_ASSERT(after ? (!std::get<1>(site) && !std::get<2>(site)) :
            (std::get<1>(site) && std::get<2>(site)));
        if (after) {
            finalizersAfter[after].push_back(allocation.GetResult());
            continue;
        }
        if (!AddFinalizerEdge(std::get<1>(site), std::get<2>(site), allocation.GetResult())) {
            return false;
        }
    }
    return true;
}

bool LocalFinalizers::AddFinalizerEdge(
    const CHIR::Block* predecessor, const CHIR::Block* successor, const CHIR::Value* object)
{
    auto edge = std::find_if(finalizerEdges.begin(), finalizerEdges.end(), [&](const Edge& item) {
        return item.predecessor == predecessor && item.successor == successor;
    });
    if (edge == finalizerEdges.end()) {
        auto label = context.GetAssembler().newLabel();
        if (!predecessor || !successor || !label.isValid()) {
            context.Fail(JitFailureKind::BlockLabelCreationFailed);
            return false;
        }
        finalizerEdges.push_back({predecessor, successor, label, {}});
        edge = std::prev(finalizerEdges.end());
    }
    edge->objects.push_back(object);
    return true;
}

void LocalFinalizers::SetCurrentBlock(const CHIR::Block& block) noexcept
{
    currentBlock = &block;
}

bool LocalFinalizers::EmitRemovals(
    const std::vector<const CHIR::Value*>& objects, const CHIR::Expression& next)
{
    if (objects.empty()) {
        return true;
    }
    auto resolved = context.GetGlobalContext().GetRuntimeBindings().GetRemoveLocalFinalizer();
    if (!resolved) {
        context.Fail(JitFailureKind::UnresolvedSymbol);
        return false;
    }
    for (auto it = objects.rbegin(); it != objects.rend(); ++it) {
        if (!MemoryUtils::LoadValue(context, *it, asmjit::x86::rdi)) {
            return false;
        }
        auto& assembler = context.GetAssembler();
        assembler.long_().mov(asmjit::x86::r11, asmjit::imm(reinterpret_cast<uintptr_t>(resolved.Value())));
        assembler.call(asmjit::x86::r11);
        if (!context.GetSafepointRecorder().Record(next)) {
            return false;
        }
    }
    return true;
}

bool LocalFinalizers::EmitAfter(const CHIR::Expression& expression, const CHIR::Expression& next)
{
    const auto found = finalizersAfter.find(&expression);
    return found == finalizersAfter.end() || EmitRemovals(found->second, next);
}

asmjit::Label LocalFinalizers::GetEdgeLabel(const CHIR::Block* successor)
{
    for (const auto& edge : finalizerEdges) {
        if (edge.predecessor == currentBlock && edge.successor == successor) {
            return edge.label;
        }
    }
    return context.GetBlockLabel(successor);
}

bool LocalFinalizers::EmitEdges()
{
    for (const auto& edge : finalizerEdges) {
        context.GetAssembler().bind(edge.label);
        const auto expressions = edge.successor->GetExpressions();
        if (expressions.empty() || !EmitRemovals(edge.objects, *expressions.front())) {
            context.Fail(JitFailureKind::UnsupportedLocalFinalizer);
            return false;
        }
        context.GetAssembler().jmp(context.GetBlockLabel(edge.successor));
    }
    return context.GetError() == asmjit::kErrorOk;
}
} // namespace Cangjie::JIT::X86_64
