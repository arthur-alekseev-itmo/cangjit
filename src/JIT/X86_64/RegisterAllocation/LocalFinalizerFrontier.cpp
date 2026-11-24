// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "LocalFinalizerFrontier.h"

#include <algorithm>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Liveness.h"
#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Expression/Terminator.h"
#include "cangjie/CHIR/IR/Value/Value.h"

namespace Cangjie::JIT::X86_64 {
namespace {
bool IsDebug(const CHIR::Expression& expression)
{
    return expression.GetExprKind() == CHIR::ExprKind::DEBUGEXPR;
}

const CHIR::Value* FindStackSlot(const CHIR::Value* value)
{
    while (value && value->IsLocalVar()) {
        const auto* expr = static_cast<const CHIR::LocalVar*>(value)->GetExpr();
        if (!expr) {
            return nullptr;
        }
        if (expr->GetExprKind() == CHIR::ExprKind::ALLOCATE) {
            const auto* type = static_cast<const CHIR::Allocate*>(expr)->GetType();
            return type && !type->IsClass() ? value : nullptr;
        }
        if (expr->GetExprKind() != CHIR::ExprKind::GET_ELEMENT_REF &&
            expr->GetExprKind() != CHIR::ExprKind::GET_ELEMENT_BY_NAME &&
            expr->GetExprKind() != CHIR::ExprKind::CLASS_STATIC_CAST) {
            return nullptr;
        }
        value = expr->GetOperand(0);
    }
    return nullptr;
}

bool CollectSlotAddresses(const CHIR::Value* slot, std::vector<const CHIR::Value*>& addresses)
{
    std::unordered_set<const CHIR::Value*> seen{slot};
    addresses.push_back(slot);
    for (size_t i = 0; i < addresses.size(); ++i) {
        const auto* address = addresses[i];
        for (const auto* user : address->GetUsers()) {
            if (!user || IsDebug(*user)) {
                continue;
            }
            const auto kind = user->GetExprKind();
            if (kind == CHIR::ExprKind::LOAD) {
                if (static_cast<const CHIR::Load*>(user)->GetLocation() != address) {
                    return false;
                }
            } else if (kind == CHIR::ExprKind::STORE) {
                if (static_cast<const CHIR::Store*>(user)->GetLocation() != address) {
                    return false;
                }
            } else if (kind == CHIR::ExprKind::CLASS_STATIC_CAST || kind == CHIR::ExprKind::GET_ELEMENT_REF ||
                kind == CHIR::ExprKind::GET_ELEMENT_BY_NAME) {
                const auto* result = user->GetResult();
                if (!result) {
                    return false;
                }
                if (seen.insert(result).second) {
                    addresses.push_back(result);
                }
            } else {
                return false;
            }
        }
    }
    return true;
}
}

LocalFinalizerFrontier ComputeLocalFinalizerFrontier(
    const CHIR::Function& function, const CHIR::AllocateBase& allocation, const Liveness& liveness,
    const ValueNumeration& numeration)
{
    const auto* body = function.GetBody();
    const auto* object = allocation.GetResult();
    if (!body || !body->GetEntryBlock() || !object || !allocation.GetParentBlock()) {
        return LocalFinalizerFrontier::Failure("invalid local finalizer allocation");
    }

    // Mirror native CodeGen's representative closure, including trackable
    // stack-slot round trips. Escapes remain registered until the region ends.
    std::vector<const CHIR::Value*> pending{object};
    std::unordered_set<const CHIR::Value*> representatives{object};
    std::unordered_set<const CHIR::Value*> visitedSlots;
    const auto addRepresentative = [&](const CHIR::Value* value, bool follow) {
        if (value && representatives.insert(value).second && follow) {
            pending.push_back(value);
        }
    };
    for (size_t i = 0; i < pending.size(); ++i) {
        const auto* value = pending[i];
        for (const auto* user : value->GetUsers()) {
            if (!user || IsDebug(*user)) {
                continue;
            }
            if (user->GetExprKind() == CHIR::ExprKind::STORE &&
                static_cast<const CHIR::Store*>(user)->GetValue() == value) {
                const auto* slot = FindStackSlot(static_cast<const CHIR::Store*>(user)->GetLocation());
                if (!slot) {
                    return LocalFinalizerFrontier::Success({});
                }
                if (!visitedSlots.insert(slot).second) {
                    continue;
                }
                std::vector<const CHIR::Value*> addresses;
                if (!CollectSlotAddresses(slot, addresses)) {
                    return LocalFinalizerFrontier::Success({});
                }
                for (const auto* address : addresses) {
                    addRepresentative(address, false);
                    for (const auto* slotUser : address->GetUsers()) {
                        if (slotUser && slotUser->GetExprKind() == CHIR::ExprKind::LOAD &&
                            static_cast<const CHIR::Load*>(slotUser)->GetLocation() == address) {
                            addRepresentative(slotUser->GetResult(), true);
                        }
                    }
                }
                continue;
            }
            if (user->GetExprKind() == CHIR::ExprKind::CLASS_STATIC_CAST ||
                user->GetExprKind() == CHIR::ExprKind::GET_ELEMENT_REF ||
                user->GetExprKind() == CHIR::ExprKind::GET_ELEMENT_BY_NAME) {
                addRepresentative(user->GetResult(), true);
            }
        }
    }

    const auto& blocks = liveness.GetBlocks();
    const auto entry = liveness.GetBlockIndex(body->GetEntryBlock());
    const auto allocationBlock = liveness.GetBlockIndex(allocation.GetParentBlock());
    if (!entry || !allocationBlock) {
        return LocalFinalizerFrontier::Failure("local finalizer has no function entry or allocation block");
    }
    std::vector<std::vector<size_t>> successors(blocks.size()), predecessors(blocks.size());
    for (size_t i = 0; i < blocks.size(); ++i) {
        for (const auto* successor : blocks[i]->GetSuccessors()) {
            const auto index = liveness.GetBlockIndex(successor);
            if (!index) {
                return LocalFinalizerFrontier::Failure("local finalizer successor is outside its function");
            }
            successors[i].push_back(*index);
            predecessors[*index].push_back(i);
        }
    }
    std::vector<unsigned char> reachable(blocks.size());
    std::vector<size_t> work{*entry};
    reachable[*entry] = 1;
    for (size_t i = 0; i < work.size(); ++i) {
        for (const auto next : successors[work[i]]) {
            if (!reachable[next]) {
                reachable[next] = 1;
                work.push_back(next);
            }
        }
    }
    if (!reachable[*allocationBlock]) {
        return LocalFinalizerFrontier::Success({});
    }

    // A dominated block can still be reached through a failed TryAllocate.
    // Track whether registration succeeded on every incoming path.
    std::vector<unsigned char> registered(blocks.size(), 1);
    registered[*entry] = 0;
    const auto edgeAvailable = [&](size_t pred, size_t succ) {
        if (pred != *allocationBlock) {
            return registered[pred] != 0;
        }
        return allocation.GetExprKind() != CHIR::ExprKind::TRY_ALLOCATE ||
            blocks[succ] != static_cast<const CHIR::TryAllocate&>(allocation).GetErrorBlock();
    };
    bool changed;
    do {
        changed = false;
        for (const auto b : work) {
            if (b == *entry) {
                continue;
            }
            bool available = true;
            for (const auto pred : predecessors[b]) {
                if (reachable[pred] && !edgeAvailable(pred, b)) {
                    available = false;
                    break;
                }
            }
            if (registered[b] != static_cast<unsigned char>(available)) {
                registered[b] = static_cast<unsigned char>(available);
                changed = true;
            }
        }
    } while (changed);

    std::vector<ValueNumeration::Id> representativeIds;
    representativeIds.reserve(representatives.size());
    for (const auto* value : representatives) {
        const auto id = numeration.GetId(value);
        if (!id) {
            return LocalFinalizerFrontier::Failure("unnumbered local finalizer representative");
        }
        representativeIds.push_back(*id);
    }
    std::vector<unsigned char> liveIn(blocks.size());
    for (const auto b : work) {
        const auto* bitmap = liveness.GetLiveIn(blocks[b]);
        liveIn[b] = std::any_of(representativeIds.begin(), representativeIds.end(),
            [bitmap](ValueNumeration::Id id) { return Liveness::Contains(bitmap, id); });
    }
    std::vector<LocalFinalizerSite> sites;
    for (const auto b : work) {
        const auto& next = successors[b];
        if (std::none_of(next.begin(), next.end(), [&](size_t successor) { return liveIn[successor]; })) {
            const CHIR::Expression* last = nullptr;
            bool allocationSeen = blocks[b] != allocation.GetParentBlock();
            for (const auto* expression : blocks[b]->GetExpressions()) {
                if (expression == &allocation) {
                    allocationSeen = true;
                }
                if (IsDebug(*expression)) {
                    continue;
                }
                bool activity = representatives.count(expression->GetResult()) != 0;
                for (size_t i = 0; !activity && i < expression->GetNumOfNonSuccessorOperands(); ++i) {
                    activity = representatives.count(expression->GetOperand(i)) != 0;
                }
                if (activity && allocationSeen) {
                    last = expression;
                }
            }
            if (!last || (b != *allocationBlock && !registered[b])) {
                continue;
            }
            if (last != blocks[b]->GetTerminator()) {
                sites.emplace_back(last, nullptr, nullptr);
            } else {
                for (const auto successor : successors[b]) {
                    if (edgeAvailable(b, successor)) {
                        sites.emplace_back(nullptr, blocks[b], blocks[successor]);
                    }
                }
            }
        } else {
            for (const auto successor : successors[b]) {
                if (!liveIn[successor] && edgeAvailable(b, successor)) {
                    sites.emplace_back(nullptr, blocks[b], blocks[successor]);
                }
            }
        }
    }
    return LocalFinalizerFrontier::Success(std::move(sites));
}
} // namespace Cangjie::JIT::X86_64
