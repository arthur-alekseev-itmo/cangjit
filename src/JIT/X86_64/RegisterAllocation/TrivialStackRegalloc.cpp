// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "TrivialStackRegalloc.h"

#include <cassert>
#include <utility>
#include <vector>

#include "InterferenceGraph.h"
#include "ValueNumeration.h"

namespace Cangjie::JIT::X86_64 {

RegisterAllocation TrivialStackRegalloc::Allocate(
    const ValueNumeration& numeration, const InterferenceGraph& interferenceGraph) const
{
    const auto valueCount = numeration.GetValueCount();
    assert(interferenceGraph.GetNodeCount() == valueCount);
    (void)interferenceGraph;

    std::vector<Location> locations;
    locations.reserve(valueCount);
    for (ValueNumeration::Id valueId = 0; valueId < valueCount; ++valueId) {
        locations.push_back(Location::Stack(static_cast<Location::Index>(valueId)));
    }
    return RegisterAllocation(std::move(locations), valueCount);
}

} // namespace Cangjie::JIT::X86_64
