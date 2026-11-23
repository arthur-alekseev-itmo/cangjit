// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "SafepointRecorder.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <utility>

#include "FunctionContext.h"
#include "GlobalContext.h"
#include "../../RegisterAllocation/SafepointLiveness.h"
#include "../../Utils/StackFrameUtils.h"
#include "cangjie/CHIR/IR/Expression/Expression.h"

namespace Cangjie::JIT::X86_64 {
bool SafepointRecorder::Record(const CHIR::Expression& expression, const CHIR::Value* temporaryRoot)
{
    const auto offset = context.GetAssembler().offset();
    if (offset < 4 || offset - 4 > std::numeric_limits<uint32_t>::max()) {
        context.Fail(JitFailureKind::UnsupportedExpression, expression.GetExprKind());
        return false;
    }
    const auto& numeration = context.GetValueNumeration();
    const auto& allocation = context.GetRegisterAllocation();
    auto result = ComputeSafepointLiveness(
        *context.GetFunction(), numeration, allocation, context.GetLivenessAnalysis(), expression);
    if (!result) {
        context.Fail(JitFailureKind::UnsupportedExpression, expression.GetExprKind());
        return false;
    }
    auto& [gc, stack, derived] = result.Value();
    if (temporaryRoot) {
        const auto id = numeration.GetId(temporaryRoot);
        const auto* slot = id && *id < allocation.GetLocationCount()
            ? allocation.GetLocation(*id).AsStack() : nullptr;
        const auto rootOffset = slot
            ? StackFrameUtils::GetSlotOffset(slot->index, allocation.GetStackSlotCount()) : std::nullopt;
        if (!slot || !rootOffset || gc.registers != 0) {
            context.Fail(JitFailureKind::UnsupportedExpression, expression.GetExprKind());
            return false;
        }
        for (ValueNumeration::Id i = 0; i < allocation.GetLocationCount(); ++i) {
            const auto* other = allocation.GetLocation(i).AsStack();
            if ((i != *id && other && other->index == slot->index) ||
                allocation.GetAllocationSlot(i) == slot->index) {
                context.Fail(JitFailureKind::UnsupportedExpression, expression.GetExprKind());
                return false;
            }
        }
        auto position = std::lower_bound(gc.slots.begin(), gc.slots.end(), *rootOffset, std::greater<int32_t>());
        const auto index = position - gc.slots.begin();
        gc.slots.insert(position, *rootOffset);
        derived.insert(derived.begin() + index, JitMetadataBuilder::Roots{});
    }
    if (!context.GetGlobalContext().IsStackGrowthEnabled()) {
        stack = {};
    }
    context.GetMetadataBuilder().RecordSafepoint(static_cast<uint32_t>(offset - 4), gc.registers, std::move(gc.slots),
        stack.registers, std::move(stack.slots), 0, std::move(derived));
    return true;
}
} // namespace Cangjie::JIT::X86_64
