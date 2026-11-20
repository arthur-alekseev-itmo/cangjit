// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "SafepointLiveness.h"

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../Utils/StackFrameUtils.h"
#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Value/Value.h"

namespace Cangjie::JIT::X86_64 {
namespace {
using Id = ValueNumeration::Id;
using Bits = std::vector<unsigned char>;

void Transfer(const CHIR::Expression& expr, const ValueNumeration& numbers, Bits& live)
{
    if (const auto* result = expr.GetResult()) {
        if (auto id = numbers.GetId(result)) {
            live[*id] = 0;
        }
    }
    for (size_t i = 0; i < expr.GetNumOfNonSuccessorOperands(); ++i) {
        if (auto id = numbers.GetId(expr.GetOperand(i))) {
            live[*id] = 1;
        }
    }
}

bool IsObjectType(const CHIR::Type& type)
{
    if (!type.IsRef()) {
        return false;
    }
    const auto* base = static_cast<const CHIR::RefType&>(type).GetBaseType();
    return base && (base->IsClass() || base->IsRawArray() || base->IsBox());
}
} // namespace

SafepointLivenessResult ComputeSafepointLiveness(const CHIR::Function& function,
    const ValueNumeration& numbers, const RegisterAllocation& allocation, const CHIR::Expression& target)
{
    const auto* body = function.GetBody();
    const size_t count = numbers.GetValueCount();
    if (!body || allocation.GetLocationCount() != count) {
        return std::string("invalid function or allocation for safepoint liveness");
    }
    // The analysis relies on a value's slot retaining its value after its last
    // ordinary use, e.g. when it is the base of a live derived pointer.
    std::unordered_set<size_t> slots;
    for (Id id = 0; id < count; ++id) {
        const auto* slot = allocation.GetLocation(id).AsStack();
        if (!slot || !slots.insert(slot->index).second ||
            !StackFrameUtils::GetSlotOffset(slot->index, allocation.GetStackSlotCount())) {
            return std::string("safepoint liveness requires distinct valid stack locations");
        }
    }
    for (Id id = 0; id < count; ++id) {
        if (auto slot = allocation.GetAllocationSlot(id)) {
            if (!slots.insert(*slot).second || !StackFrameUtils::GetSlotOffset(*slot, allocation.GetStackSlotCount())) {
                return std::string("invalid or overlapping stack allocation slot");
            }
        }
    }
    auto blocks = body->GetAllBlocks();
    blocks.erase(std::remove_if(blocks.begin(), blocks.end(),
        [&](const CHIR::Block* b) { return b->GetFuncOrLambdaBody() != body; }), blocks.end());
    std::unordered_map<const CHIR::Block*, size_t> index;
    for (size_t i = 0; i < blocks.size(); ++i) {
        index.emplace(blocks[i], i);
    }
    const auto entry = index.find(body->GetEntryBlock());
    if (entry == index.end()) {
        return std::string("function has no entry block");
    }
    std::vector<std::vector<size_t>> successors(blocks.size()), predecessors(blocks.size());
    for (size_t i = 0; i < blocks.size(); ++i) {
        for (const auto* successor : blocks[i]->GetSuccessors()) {
            auto found = index.find(successor);
            if (found == index.end()) {
                return std::string("CFG successor outside function");
            }
            successors[i].push_back(found->second);
            predecessors[found->second].push_back(i);
        }
    }
    std::vector<unsigned char> reachable(blocks.size());
    std::vector<size_t> work{entry->second};
    reachable[entry->second] = 1;
    for (size_t n = 0; n < work.size(); ++n) {
        for (auto next : successors[work[n]]) {
            if (!reachable[next]) {
                reachable[next] = 1;
                work.push_back(next);
            }
        }
    }
    auto targetBlock = index.find(target.GetParentBlock());
    if (targetBlock == index.end() || !reachable[targetBlock->second]) {
        return std::string("safepoint is not in a reachable block");
    }
    for (auto b : work) {
        for (const auto* expr : blocks[b]->GetExpressions()) {
            const auto* result = expr->GetResult();
            if (result && !numbers.GetId(result)) {
                return std::string("expression result is missing from value numeration");
            }
            for (size_t operand = 0; operand < expr->GetNumOfNonSuccessorOperands(); ++operand) {
                const auto* value = expr->GetOperand(operand);
                if (value && (value->IsLocalVar() || value->IsParameter()) && !numbers.GetId(value)) {
                    return std::string("expression operand is missing from value numeration");
                }
            }
        }
    }
    // Backward may-liveness, including all normal and exceptional successors.
    std::vector<Bits> in(blocks.size(), Bits(count)), out = in;
    bool changed;
    do {
        changed = false;
        for (auto it = work.rbegin(); it != work.rend(); ++it) {
            size_t b = *it;
            Bits next(count);
            for (auto s : successors[b]) {
                for (size_t id = 0; id < count; ++id) {
                    next[id] |= in[s][id];
                }
            }
            Bits before = next;
            auto expressions = blocks[b]->GetExpressions();
            for (auto e = expressions.rbegin(); e != expressions.rend(); ++e) {
                Transfer(**e, numbers, before);
            }
            if (next != out[b] || before != in[b]) {
                out[b] = std::move(next);
                in[b] = std::move(before);
                changed = true;
            }
        }
    } while (changed);

    // Forward must-initialization. Start at top except at entry, and intersect
    // incoming edges. A try-result exists only on the success edge.
    std::vector<Bits> initializedIn(blocks.size(), Bits(count, 1)), initializedOut = initializedIn;
    Bits parameters(count);
    for (Id id = numbers.GetParameterOffset(); id < numbers.GetLocalOffset(); ++id) {
        parameters[id] = 1;
    }
    do {
        changed = false;
        for (auto b : work) {
            Bits before(count, 1);
            if (b == entry->second) {
                before = parameters;
            } else {
                for (auto pred : predecessors[b]) {
                    if (!reachable[pred]) {
                        continue;
                    }
                    Bits edge = initializedOut[pred];
                    const auto expressions = blocks[pred]->GetExpressions();
                    if (!expressions.empty()) {
                        const auto* last = expressions.back();
                        const auto branches = last->GetSuccessAndErrorBlocks();
                        if (branches.second == blocks[b]) {
                            if (const auto* result = last->GetResult()) {
                                if (auto id = numbers.GetId(result)) {
                                    edge[*id] = 0;
                                }
                            }
                        }
                    }
                    for (size_t id = 0; id < count; ++id) {
                        before[id] &= edge[id];
                    }
                }
            }
            Bits after = before;
            for (const auto* expr : blocks[b]->GetExpressions()) {
                if (const auto* result = expr->GetResult()) {
                    if (auto id = numbers.GetId(result)) {
                        after[*id] = 1;
                    }
                }
            }
            if (before != initializedIn[b] || after != initializedOut[b]) {
                initializedIn[b] = std::move(before);
                initializedOut[b] = std::move(after);
                changed = true;
            }
        }
    } while (changed);

    const size_t b = targetBlock->second;
    Bits live = out[b], initialized = initializedIn[b];
    bool found = false;
    const auto expressions = blocks[b]->GetExpressions();
    for (auto it = expressions.rbegin(); it != expressions.rend(); ++it) {
        Transfer(**it, numbers, live);
        if (*it == &target) {
            found = true;
            break;
        }
    }
    if (!found) {
        return std::string("safepoint expression is absent from its block");
    }
    for (const auto* expr : expressions) {
        if (expr == &target) {
            break;
        }
        if (const auto* result = expr->GetResult()) {
            if (auto id = numbers.GetId(result)) {
                initialized[*id] = 1;
            }
        }
    }
    // Ensure a try-result is not used on the exception edge. Such a value is
    // uninitialized even though the ordinary transfer kills the definition.
    const auto branches = target.GetSuccessAndErrorBlocks();
    if (branches.second && target.GetResult()) {
        const auto e = index.find(branches.second);
        const auto id = numbers.GetId(target.GetResult());
        if (e != index.end() && id && in[e->second][*id]) {
            return std::string("try result is live on its exception edge");
        }
    }

    JitMetadataBuilder::Roots gc{0, {}}, stack{0, {}};
    std::unordered_map<Id, std::vector<int32_t>> derived;
    std::unordered_set<Id> roots;
    std::string error;
    auto offset = [&](Id id) { return *StackFrameUtils::GetSlotOffset(
        allocation.GetLocation(id).AsStack()->index, allocation.GetStackSlotCount()); };
    // Resolve interior references to either a stack allocation or a managed
    // object base. The base may be dead in CHIR but must still be initialized.
    std::function<int(Id, std::unordered_set<Id>&)> classify = [&](Id id, std::unordered_set<Id>& seen) -> int {
        if (!initialized[id] || !seen.insert(id).second) {
            error = "uninitialized or cyclic reference provenance at safepoint";
            return -1;
        }
        const auto* value = numbers.GetValue(id);
        const auto* type = value ? value->GetType() : nullptr;
        if (!type) {
            error = "live value has no CHIR type";
            return -1;
        }
        // Local-region objects are neither ordinary movable-heap roots nor
        // stack addresses. Their registry and lifetime are runtime-managed;
        // this stack-map format cannot identify their provenance here.
        const auto* referenceBase = type->IsRef() ? static_cast<const CHIR::RefType*>(type)->GetBaseType() : nullptr;
        if (type->IsLocalRegion() || (referenceBase && referenceBase->IsLocalRegion())) {
            error = "live local-region reference cannot be represented as a GC root";
            return -1;
        }
        const auto* expr = value->IsLocalVar() ? static_cast<const CHIR::LocalVar*>(value)->GetExpr() : nullptr;
        // Landing-pad GetException denotes the runtime's unmanaged
        // ExceptionWrapper, not the managed exception returned by begin-catch.
        if (expr && expr->GetExprKind() == CHIR::ExprKind::GET_EXCEPTION) {
            return 0;
        }
        if (expr && (expr->GetExprKind() == CHIR::ExprKind::ALLOCATE ||
                     expr->GetExprKind() == CHIR::ExprKind::TRY_ALLOCATE)) {
            const auto* base = type->IsRef() ? static_cast<const CHIR::RefType*>(type)->GetBaseType() : nullptr;
            if (base && base->IsClass()) {
                roots.insert(id);
                return 2;
            }
            if (expr->GetExprKind() != CHIR::ExprKind::ALLOCATE ||
                !base || !base->IsPrimitive() || !allocation.GetAllocationSlot(id)) {
                error = "stack allocation has unsupported or potentially managed contents";
                return -1;
            }
            return 1;
        }
        if (expr && (expr->GetExprKind() == CHIR::ExprKind::GET_ELEMENT_REF ||
                     expr->GetExprKind() == CHIR::ExprKind::GET_ELEMENT_BY_NAME)) {
            const auto base = numbers.GetId(expr->GetOperand(0));
            if (!base) {
                error = "interior reference base has no allocated location";
                return -1;
            }
            int kind = classify(*base, seen);
            if (kind == 0) {
                error = "interior reference has an unmanaged base";
                return -1;
            }
            if (kind == 2) {
                // classify() records a managed base, or a derived base. Find
                // its ultimate root, and associate this location with it.
                Id root = *base;
                while (true) {
                    const auto* v = numbers.GetValue(root);
                    const auto* e = v->IsLocalVar() ? static_cast<const CHIR::LocalVar*>(v)->GetExpr() : nullptr;
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
        if (type->IsRef() || type->IsCPointer() || type->IsCString()) {
            error = "reference origin cannot be represented exactly";
            return -1;
        }
        if (!type->IsPrimitive() && !type->IsVoid() && !type->IsNothing()) {
            error = "live aggregate or unknown CHIR type requires a GC layout";
            return -1;
        }
        return 0;
    };
    for (Id id = 0; id < count; ++id) {
        if (!live[id]) {
            continue;
        }
        std::unordered_set<Id> seen;
        int kind = classify(id, seen);
        if (kind < 0) {
            return error;
        }
        if (kind == 1) {
            stack.second.push_back(offset(id));
        }
    }
    for (auto id : roots) {
        gc.second.push_back(offset(id));
    }
    auto sortUnique = [](std::vector<int32_t>& values) {
        std::sort(values.begin(), values.end(), std::greater<int32_t>());
        values.erase(std::unique(values.begin(), values.end()), values.end());
    };
    sortUnique(gc.second);
    sortUnique(stack.second);
    JitMetadataBuilder::DerivedRoots perRoot;
    perRoot.reserve(gc.second.size());
    for (auto slot : gc.second) {
        auto it = std::find_if(roots.begin(), roots.end(), [&](Id id) { return offset(id) == slot; });
        auto values = derived[*it];
        sortUnique(values);
        perRoot.emplace_back(0, std::move(values));
    }
    return SafepointRoots{std::move(gc), std::move(stack), std::move(perRoot)};
}
} // namespace Cangjie::JIT::X86_64
