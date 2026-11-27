// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "SafepointRecorder.h"

#include "FunctionContext.h"
#include "GlobalContext.h"
#include "../../JitAssert.h"
#include "../../RegisterAllocation/SafepointLiveness.h"
#include "../../Utils/StackFrameUtils.h"
#include "cangjie/CHIR/IR/Expression/Expression.h"

#include <functional>
#include <limits>

namespace Cangjie::JIT::X86_64 {
void SafepointRecorder::Record(const CHIR::Expression& expression, const CHIR::Value* temporaryRoot)
{
    const auto offset = context.GetAssembler().offset();
    JIT_ASSERT(offset >= 4 && offset - 4 <= std::numeric_limits<uint32_t>::max(),
        "safepoint program counter is out of range");
    auto roots = ComputeSafepointLiveness(
        *context.GetFunction(), context.GetValueNumeration(), context.GetRegisterAllocation(),
        context.GetLiveness(), expression);
    if (temporaryRoot) {
        auto& numeration = context.GetValueNumeration();
        auto& allocation = context.GetRegisterAllocation();
        const auto id = numeration.GetId(temporaryRoot);
        JIT_ASSERT(id && *id < allocation.GetLocationCount(), "temporary root is numbered");
        const auto slot = allocation.GetLocation(*id).AsStack();
        JIT_ASSERT(slot != nullptr, "temporary root occupies a stack slot");
        for (ValueNumeration::Id i = 0; i < allocation.GetLocationCount(); ++i) {
            auto other = allocation.GetLocation(i).AsStack();
            JIT_ASSERT(i == *id || !other || other->index != slot->index,
                "temporary root must not share a slot with a live value");
            JIT_ASSERT(allocation.GetAllocationSlot(i) != slot->index,
                "temporary root must not overlap an allocation slot");
        }
        const auto rootOffset =
            StackFrameUtils::GetSlotOffset(slot->index, allocation.GetStackSlotCount());
        JIT_ASSERT(roots.gcRoots.registers == 0, "temporary root requires free GC registers");
        auto position = std::lower_bound(
            roots.gcRoots.slots.begin(), roots.gcRoots.slots.end(), rootOffset, std::greater<int32_t>());
        const auto index = position - roots.gcRoots.slots.begin();
        roots.gcRoots.slots.insert(position, rootOffset);
        roots.derivedRoots.insert(roots.derivedRoots.begin() + index, FunctionMetadata::Roots{});
    }
    if (!context.GetGlobalContext().IsStackGrowthEnabled()) {
        roots.stackRoots = {};
    }
    context.GetMetadata().RecordSafepoint(static_cast<uint32_t>(offset - 4),
        roots.gcRoots.registers, std::move(roots.gcRoots.slots),
        roots.stackRoots.registers, std::move(roots.stackRoots.slots), 0, std::move(roots.derivedRoots));
}
} // namespace Cangjie::JIT::X86_64
