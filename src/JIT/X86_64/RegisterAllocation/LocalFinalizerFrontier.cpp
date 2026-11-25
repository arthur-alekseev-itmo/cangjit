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

struct Representatives {
    std::vector<const CHIR::Value*> values;
    std::unordered_set<const CHIR::Value*> members;
};

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

void AddRepresentative(Representatives& representatives, const CHIR::Value* value, bool follow)
{
    if (value && representatives.members.insert(value).second) {
        if (follow) {
            representatives.values.push_back(value);
        }
    }
}

bool DiscoverRepresentatives(const CHIR::Value* object, Representatives& representatives)
{
    AddRepresentative(representatives, object, true);
    std::unordered_set<const CHIR::Value*> visitedSlots;
    for (size_t i = 0; i < representatives.values.size(); ++i) {
        const auto* value = representatives.values[i];
        for (const auto* user : value->GetUsers()) {
            if (!user || IsDebug(*user)) {
                continue;
            }
            if (user->GetExprKind() == CHIR::ExprKind::STORE &&
                static_cast<const CHIR::Store*>(user)->GetValue() == value) {
                const auto* slot = FindStackSlot(static_cast<const CHIR::Store*>(user)->GetLocation());
                if (!slot) {
                    return false;
                }
                if (!visitedSlots.insert(slot).second) {
                    continue;
                }
                std::vector<const CHIR::Value*> addresses;
                if (!CollectSlotAddresses(slot, addresses)) {
                    return false;
                }
                for (const auto* address : addresses) {
                    AddRepresentative(representatives, address, false);
                    for (const auto* slotUser : address->GetUsers()) {
                        if (slotUser && slotUser->GetExprKind() == CHIR::ExprKind::LOAD &&
                            static_cast<const CHIR::Load*>(slotUser)->GetLocation() == address) {
                            AddRepresentative(representatives, slotUser->GetResult(), true);
                        }
                    }
                }
                continue;
            }
            if (user->GetExprKind() == CHIR::ExprKind::CLASS_STATIC_CAST ||
                user->GetExprKind() == CHIR::ExprKind::GET_ELEMENT_REF ||
                user->GetExprKind() == CHIR::ExprKind::GET_ELEMENT_BY_NAME) {
                AddRepresentative(representatives, user->GetResult(), true);
            }
        }
    }
    return true;
}

struct ControlFlow {
    std::vector<std::vector<size_t>> successors;
    std::vector<std::vector<size_t>> predecessors;
    std::vector<size_t> reachable;
};

bool BuildControlFlow(const CHIR::Function& function, const Liveness& liveness,
    ControlFlow& flow, size_t& entry, size_t& allocationBlock, const CHIR::AllocateBase& allocation)
{
    const auto entryIndex = liveness.GetBlockIndex(function.GetBody()->GetEntryBlock());
    const auto allocationIndex = liveness.GetBlockIndex(allocation.GetParentBlock());
    if (!entryIndex || !allocationIndex) {
        return false;
    }
    entry = *entryIndex;
    allocationBlock = *allocationIndex;
    const auto& blocks = liveness.GetBlocks();
    flow.successors.resize(blocks.size());
    flow.predecessors.resize(blocks.size());
    for (size_t i = 0; i < blocks.size(); ++i) {
        for (const auto* successor : blocks[i]->GetSuccessors()) {
            const auto index = liveness.GetBlockIndex(successor);
            if (!index) {
                return false;
            }
            flow.successors[i].push_back(*index);
            flow.predecessors[*index].push_back(i);
        }
    }
    std::vector<unsigned char> seen(blocks.size());
    flow.reachable.push_back(entry);
    seen[entry] = 1;
    for (size_t i = 0; i < flow.reachable.size(); ++i) {
        for (const auto next : flow.successors[flow.reachable[i]]) {
            if (!seen[next]) {
                seen[next] = 1;
                flow.reachable.push_back(next);
            }
        }
    }
    return true;
}

bool IsRegistrationAvailable(const CHIR::AllocateBase& allocation, const Liveness& liveness,
    const std::vector<unsigned char>& registered, size_t predecessor, size_t successor,
    size_t allocationBlock)
{
    if (predecessor != allocationBlock) {
        return registered[predecessor] != 0;
    }
    return allocation.GetExprKind() != CHIR::ExprKind::TRY_ALLOCATE ||
        liveness.GetBlocks()[successor] != static_cast<const CHIR::TryAllocate&>(allocation).GetErrorBlock();
}

std::vector<unsigned char> ComputeRegistrationState(const CHIR::AllocateBase& allocation,
    const Liveness& liveness, const ControlFlow& flow, size_t entry, size_t allocationBlock)
{
    const auto& blocks = liveness.GetBlocks();
    std::vector<unsigned char> registered(blocks.size(), 1);
    registered[entry] = 0;
    bool changed;
    do {
        changed = false;
        for (const auto block : flow.reachable) {
            if (block == entry) {
                continue;
            }
            bool available = true;
            for (const auto pred : flow.predecessors[block]) {
                if (std::find(flow.reachable.begin(), flow.reachable.end(), pred) != flow.reachable.end() &&
                    !IsRegistrationAvailable(allocation, liveness, registered, pred, block, allocationBlock)) {
                    available = false;
                    break;
                }
            }
            if (registered[block] != static_cast<unsigned char>(available)) {
                registered[block] = static_cast<unsigned char>(available);
                changed = true;
            }
        }
    } while (changed);
    return registered;
}

bool ComputeLiveIn(const Representatives& representatives, const ValueNumeration& numeration,
    const Liveness& liveness, const ControlFlow& flow, std::vector<unsigned char>& liveIn)
{
    std::vector<ValueNumeration::Id> ids;
    ids.reserve(representatives.members.size());
    for (const auto* value : representatives.members) {
        const auto id = numeration.GetId(value);
        if (!id) {
            return false;
        }
        ids.push_back(*id);
    }
    liveIn.resize(liveness.GetBlocks().size());
    for (const auto block : flow.reachable) {
        const auto* bitmap = liveness.GetLiveIn(liveness.GetBlocks()[block]);
        liveIn[block] = std::any_of(ids.begin(), ids.end(),
            [bitmap](ValueNumeration::Id id) { return Liveness::Contains(bitmap, id); });
    }
    return true;
}

void AddBlockFinalizers(const CHIR::Block* block, const CHIR::AllocateBase& allocation,
    const Representatives& representatives, const std::vector<size_t>& successors,
    const std::vector<unsigned char>& liveIn, const std::vector<unsigned char>& registered,
    size_t blockIndex, size_t allocationBlock, const Liveness& liveness,
    std::vector<LocalFinalizerSite>& sites)
{
    const auto hasLiveSuccessor = std::any_of(successors.begin(), successors.end(),
        [&liveIn](size_t successor) { return liveIn[successor] != 0; });
    const auto edgeAvailable = [&](size_t successor) {
        return IsRegistrationAvailable(allocation, liveness, registered, blockIndex, successor,
            allocationBlock);
    };
    if (!hasLiveSuccessor) {
        const CHIR::Expression* last = nullptr;
        bool allocationSeen = block != allocation.GetParentBlock();
        for (const auto* expression : block->GetExpressions()) {
            if (expression == &allocation) {
                allocationSeen = true;
            }
            if (IsDebug(*expression)) {
                continue;
            }
            bool activity = representatives.members.count(expression->GetResult()) != 0;
            for (size_t i = 0; !activity && i < expression->GetNumOfNonSuccessorOperands(); ++i) {
                activity = representatives.members.count(expression->GetOperand(i)) != 0;
            }
            if (activity && allocationSeen) {
                last = expression;
            }
        }
        if (!last || (block != allocation.GetParentBlock() && !registered[blockIndex])) {
            return;
        }
        if (last != block->GetTerminator()) {
            sites.push_back({last, nullptr, nullptr});
            return;
        }
        for (const auto successor : successors) {
            if (edgeAvailable(successor)) {
                sites.push_back({nullptr, block, liveness.GetBlocks()[successor]});
            }
        }
        return;
    }
    for (const auto successor : successors) {
        if (!liveIn[successor] && edgeAvailable(successor)) {
            sites.push_back({nullptr, block, liveness.GetBlocks()[successor]});
        }
    }
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

    // Mirror native CodeGen's representative closure; unknown escapes live to region end.
    Representatives representatives;
    if (!DiscoverRepresentatives(object, representatives)) {
        return LocalFinalizerFrontier::Success({});
    }

    ControlFlow flow;
    size_t entry = 0;
    size_t allocationBlock = 0;
    if (!BuildControlFlow(function, liveness, flow, entry, allocationBlock, allocation)) {
        return LocalFinalizerFrontier::Failure("local finalizer has no function entry or allocation block");
    }
    if (std::find(flow.reachable.begin(), flow.reachable.end(), allocationBlock) == flow.reachable.end()) {
        return LocalFinalizerFrontier::Success({});
    }

    // Failed TryAllocate edges do not carry a registered finalizer.
    const auto registered = ComputeRegistrationState(allocation, liveness, flow, entry, allocationBlock);
    std::vector<unsigned char> liveIn;
    if (!ComputeLiveIn(representatives, numeration, liveness, flow, liveIn)) {
        return LocalFinalizerFrontier::Failure("unnumbered local finalizer representative");
    }
    std::vector<LocalFinalizerSite> sites;
    const auto& blocks = liveness.GetBlocks();
    for (const auto block : flow.reachable) {
        AddBlockFinalizers(blocks[block], allocation, representatives, flow.successors[block],
            liveIn, registered, block, allocationBlock, liveness, sites);
    }
    return LocalFinalizerFrontier::Success(std::move(sites));
}
} // namespace Cangjie::JIT::X86_64

