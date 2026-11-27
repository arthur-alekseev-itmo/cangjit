// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "SafepointLiveness.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Liveness.h"
#include "../JitAssert.h"
#include "../Utils/StackFrameUtils.h"
#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Value/Value.h"

namespace Cangjie::JIT::X86_64 {
namespace {
using Id = ValueNumeration::Id;
using Bits = std::vector<unsigned char>;

bool IsObjectType(const CHIR::Type& type)
{
    if (!type.IsRef()) {
        return false;
    }
    auto base = static_cast<const CHIR::RefType&>(type).GetBaseType();
    return base && (base->IsClass() || base->IsRawArray() || base->IsBox());
}

std::optional<Id> GetExceptionalResult(const CHIR::Block& predecessor, const CHIR::Block& successor,
    const ValueNumeration& numbers)
{
    const auto count = predecessor.GetExpressionsNum();
    if (count == 0) {
        return std::nullopt;
    }
    auto expression = predecessor.GetExpressionByIdx(count - 1);
    if (expression->GetSuccessAndErrorBlocks().second != &successor) {
        return std::nullopt;
    }
    auto result = expression->GetResult();
    return result ? numbers.GetId(result) : std::nullopt;
}

// Forward must-initialization, intersecting incoming edges.
Bits ComputeInitializedAtBlock(const std::vector<CHIR::Block*>& blocks,
    const std::vector<std::vector<size_t>>& predecessors, const std::vector<unsigned char>& reachable,
    const std::vector<size_t>& work, size_t entry, size_t target, const ValueNumeration& numbers)
{
    const size_t count = numbers.GetValueCount();
    std::vector<Bits> initializedIn(blocks.size(), Bits(count, 1)), initializedOut = initializedIn;
    Bits parameters(count);
    for (Id id = numbers.GetParameterOffset(); id < numbers.GetLocalOffset(); ++id) {
        parameters[id] = 1;
    }
    bool changed;
    do {
        changed = false;
        for (const auto block : work) {
            Bits before(count, 1);
            if (block == entry) {
                before = parameters;
            } else {
                for (const auto pred : predecessors[block]) {
                    if (!reachable[pred]) {
                        continue;
                    }
                    for (size_t id = 0; id < count; ++id) {
                        before[id] &= initializedOut[pred][id];
                    }
                    if (const auto result = GetExceptionalResult(*blocks[pred], *blocks[block], numbers)) {
                        before[*result] = 0;
                    }
                }
            }
            Bits after = before;
            for (size_t i = 0; i < blocks[block]->GetExpressionsNum(); ++i) {
                if (auto result = blocks[block]->GetExpressionByIdx(i)->GetResult()) {
                    if (const auto id = numbers.GetId(result)) {
                        after[*id] = 1;
                    }
                }
            }
            if (before != initializedIn[block] || after != initializedOut[block]) {
                initializedIn[block] = std::move(before);
                initializedOut[block] = std::move(after);
                changed = true;
            }
        }
    } while (changed);
    return std::move(initializedIn[target]);
}
} // namespace

SafepointRoots ComputeSafepointLiveness(const CHIR::Function& function,
    const ValueNumeration& numbers, const RegisterAllocation& allocation, const Liveness& liveness,
    const CHIR::Expression& target)
{
    auto body = function.GetBody();
    const size_t count = numbers.GetValueCount();
    JIT_ASSERT(body != nullptr && allocation.GetLocationCount() == count,
        "safepoint liveness runs on a complete allocation");
    // The analysis relies on a value's slot retaining its value after its last
    // ordinary use, e.g. when it is the base of a live derived pointer.
    std::unordered_set<size_t> slots;
    for (Id id = 0; id < count; ++id) {
        auto slot = allocation.GetLocation(id).AsStack();
        JIT_ASSERT(slot != nullptr && slots.insert(slot->index).second,
            "safepoint liveness requires distinct stack locations");
        StackFrameUtils::GetSlotOffset(slot->index, allocation.GetStackSlotCount());
    }
    for (Id id = 0; id < count; ++id) {
        if (auto slot = allocation.GetAllocationSlot(id)) {
            JIT_ASSERT(slots.insert(*slot).second, "stack allocation slot is disjoint from value slots");
            StackFrameUtils::GetSlotOffset(*slot, allocation.GetStackSlotCount());
        }
    }
    const auto& blocks = liveness.GetBlocks();
    // GetBlockIndex asserts that the block belongs to the analyzed function.
    const auto entry = liveness.GetBlockIndex(body->GetEntryBlock());
    std::vector<std::vector<size_t>> successors(blocks.size()), predecessors(blocks.size());
    for (size_t i = 0; i < blocks.size(); ++i) {
        for (auto successor : blocks[i]->GetSuccessors()) {
            const auto found = liveness.GetBlockIndex(successor);
            successors[i].push_back(found);
            predecessors[found].push_back(i);
        }
    }
    std::vector<unsigned char> reachable(blocks.size());
    std::vector<size_t> work{entry};
    reachable[entry] = 1;
    for (size_t n = 0; n < work.size(); ++n) {
        for (auto next : successors[work[n]]) {
            if (!reachable[next]) {
                reachable[next] = 1;
                work.push_back(next);
            }
        }
    }
    const auto targetBlock = liveness.GetBlockIndex(target.GetParentBlock());
    JIT_ASSERT(reachable[targetBlock], "safepoint is in a reachable block");
    for (auto b : work) {
        for (auto expr : blocks[b]->GetExpressions()) {
            auto result = expr->GetResult();
            JIT_ASSERT(!result || numbers.GetId(result),
                "expression result is missing from value numeration");
            for (size_t operand = 0; operand < expr->GetNumOfNonSuccessorOperands(); ++operand) {
                auto value = expr->GetOperand(operand);
                JIT_ASSERT(!(value && (value->IsLocalVar() || value->IsParameter()) && !numbers.GetId(value)),
                    "expression operand is missing from value numeration");
            }
        }
    }

    const size_t b = targetBlock;
    Bits initialized = ComputeInitializedAtBlock(blocks, predecessors, reachable, work, entry, b, numbers);
    JIT_ASSERT(liveness.ContainsExpression(target), "safepoint expression belongs to its block");
    auto live = liveness.GetLiveBefore(target);
    const auto expressions = blocks[b]->GetExpressions();
    for (auto expr : expressions) {
        if (expr == &target) {
            break;
        }
        if (auto result = expr->GetResult()) {
            if (auto id = numbers.GetId(result)) {
                initialized[*id] = 1;
            }
        }
    }
    // Ensure a try-result is not used on the exception edge. Such a value is
    // uninitialized even though the ordinary transfer kills the definition.
    const auto branches = target.GetSuccessAndErrorBlocks();
    if (branches.second && target.GetResult()) {
        const auto e = liveness.GetBlockIndex(branches.second);
        const auto id = numbers.GetId(target.GetResult());
        JIT_ASSERT(!(e && id && Liveness::Contains(liveness.GetLiveIn(branches.second), *id)),
            "try result is not live on its exception edge");
    }

    FunctionMetadata::Roots gc{0, {}}, stack{0, {}};
    std::unordered_map<Id, std::vector<int32_t>> derived;
    std::unordered_set<Id> roots;
    auto offset = [&](Id id) { return StackFrameUtils::GetSlotOffset(
        allocation.GetLocation(id).AsStack()->index, allocation.GetStackSlotCount()); };
    // Resolve interior references to either a stack allocation or a managed
    // object base. The base may be dead in CHIR but must still be initialized.
    std::function<int(Id, std::unordered_set<Id>&)> classify = [&](Id id, std::unordered_set<Id>& seen) -> int {
        // A live value is initialized here unless the CHIR graph violates its
        // own data flow; reference provenance must terminate.
        JIT_ASSERT(initialized[id] && seen.insert(id).second,
            "uninitialized or cyclic reference provenance at safepoint");
        auto value = numbers.GetValue(id);
        auto type = value ? value->GetType() : nullptr;
        JIT_ASSERT(type != nullptr, "live value has a CHIR type");
        auto expr = value->IsLocalVar() ? static_cast<const CHIR::LocalVar*>(value)->GetExpr() : nullptr;
        auto referenceBase = type->IsRef() ? static_cast<const CHIR::RefType*>(type)->GetBaseType() : nullptr;
        if (expr && expr->GetExprKind() == CHIR::ExprKind::ALLOCATE && referenceBase && referenceBase->IsRef()) {
            auto storedBase = static_cast<const CHIR::RefType*>(referenceBase)->GetBaseType();
            if (storedBase && storedBase->IsClass() && storedBase->IsMustLocalRegion() &&
                allocation.GetAllocationSlot(id)) {
                // The slot address moves with the stack; its contents are stable local-region pointers.
                return 1;
            }
        }
        if (type->IsLocalRegion() || (referenceBase && referenceBase->IsLocalRegion())) {
            // Definitely local objects are stable region allocations. The
            // runtime's local-root/finalizer registry traces their heap fields.
            // A maybe-local parameter can instead be a movable heap object.
            const bool allocatedHere = expr &&
                (expr->GetExprKind() == CHIR::ExprKind::ALLOCATE ||
                    expr->GetExprKind() == CHIR::ExprKind::TRY_ALLOCATE);
            JIT_ASSERT(referenceBase && referenceBase->IsClass() &&
                (referenceBase->IsMustLocalRegion() || allocatedHere),
                "live maybe-local reference cannot be represented as a GC root");
            return 0;
        }
        // Landing-pad GetException denotes the runtime's unmanaged
        // ExceptionWrapper, not the managed exception returned by begin-catch.
        if (expr && expr->GetExprKind() == CHIR::ExprKind::GET_EXCEPTION) {
            return 0;
        }
        if (expr && (expr->GetExprKind() == CHIR::ExprKind::ALLOCATE ||
                     expr->GetExprKind() == CHIR::ExprKind::TRY_ALLOCATE)) {
            auto base = type->IsRef() ? static_cast<const CHIR::RefType*>(type)->GetBaseType() : nullptr;
            if (base && base->IsClass()) {
                roots.insert(id);
                return 2;
            }
            JIT_ASSERT(expr->GetExprKind() == CHIR::ExprKind::ALLOCATE && base && base->IsPrimitive() &&
                allocation.GetAllocationSlot(id),
                "stack allocation has unsupported or potentially managed contents");
            return 1;
        }
        if (expr && (expr->GetExprKind() == CHIR::ExprKind::GET_ELEMENT_REF ||
                     expr->GetExprKind() == CHIR::ExprKind::GET_ELEMENT_BY_NAME)) {
            const auto base = numbers.GetId(expr->GetOperand(0));
            JIT_ASSERT(base, "interior reference base has an allocated location");
            int kind = classify(*base, seen);
            JIT_ASSERT(kind != 0, "interior reference has a managed base");
            if (kind == 2) {
                // classify() records a managed base, or a derived base. Find
                // its ultimate root, and associate this location with it.
                Id root = *base;
                while (true) {
                    auto v = numbers.GetValue(root);
                    auto e = v->IsLocalVar() ? static_cast<const CHIR::LocalVar*>(v)->GetExpr() : nullptr;
                    if (!e || (e->GetExprKind() != CHIR::ExprKind::GET_ELEMENT_REF &&
                               e->GetExprKind() != CHIR::ExprKind::GET_ELEMENT_BY_NAME)) {
                        break;
                    }
                    root = *numbers.GetId(e->GetOperand(0));
                }
                derived[root].push_back(offset(id));
            }
            return kind;
        }
        if (IsObjectType(*type)) {
            roots.insert(id);
            return 2;
        }
        JIT_ASSERT(!(type->IsRef() || type->IsCPointer() || type->IsCString()),
            "reference origin cannot be represented exactly");
        JIT_ASSERT(type->IsPrimitive() || type->IsVoid() || type->IsNothing(),
            "live aggregate or unknown CHIR type requires a GC layout");
        return 0;
    };
    for (Id id = 0; id < count; ++id) {
        if (!Liveness::Contains(live, id)) {
            continue;
        }
        std::unordered_set<Id> seen;
        if (classify(id, seen) == 1) {
            stack.slots.push_back(offset(id));
        }
    }
    for (auto id : roots) {
        gc.slots.push_back(offset(id));
    }
    auto sortUnique = [](std::vector<int32_t>& values) {
        std::sort(values.begin(), values.end(), std::greater<int32_t>());
        values.erase(std::unique(values.begin(), values.end()), values.end());
    };
    sortUnique(gc.slots);
    sortUnique(stack.slots);
    FunctionMetadata::DerivedRoots perRoot;
    perRoot.reserve(gc.slots.size());
    for (auto slot : gc.slots) {
        const auto it = std::find_if(roots.begin(), roots.end(), [&](Id id) { return offset(id) == slot; });
        JIT_ASSERT(it != roots.end(), "safepoint root has a corresponding value");
        auto values = derived[*it];
        sortUnique(values);
        perRoot.push_back({0, std::move(values)});
    }
    return {std::move(gc), std::move(stack), std::move(perRoot)};
}
} // namespace Cangjie::JIT::X86_64
