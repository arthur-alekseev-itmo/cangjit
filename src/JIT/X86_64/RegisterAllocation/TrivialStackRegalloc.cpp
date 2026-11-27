// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "TrivialStackRegalloc.h"

#include <utility>
#include <vector>

#include "../JitAssert.h"
#include "InterferenceGraph.h"
#include "ValueNumeration.h"
#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Value/Value.h"

namespace Cangjie::JIT::X86_64 {

RegisterAllocation TrivialStackRegalloc::Allocate(
    const ValueNumeration& numeration, const InterferenceGraph& interferenceGraph) const
{
    const auto valueCount = numeration.GetValueCount();
    JIT_ASSERT(interferenceGraph.GetNodeCount() == valueCount,
        "interference graph covers every numbered value");
    (void)interferenceGraph;

    std::vector<Location> locations;
    locations.reserve(valueCount);
    std::vector<std::optional<size_t>> allocationSlots(valueCount);
    size_t stackSlotCount = valueCount;
    for (ValueNumeration::Id valueId = 0; valueId < valueCount; ++valueId) {
        locations.push_back(Location::Stack(static_cast<Location::Index>(valueId)));
        auto value = numeration.GetValue(valueId);
        if (value->IsLocalVar()) {
            auto expression = static_cast<const CHIR::LocalVar*>(value)->GetExpr();
            if (expression && expression->GetExprKind() == CHIR::ExprKind::ALLOCATE) {
                allocationSlots[valueId] = stackSlotCount++;
            }
        }
    }
    return RegisterAllocation(std::move(locations), stackSlotCount, std::move(allocationSlots));
}

} // namespace Cangjie::JIT::X86_64
