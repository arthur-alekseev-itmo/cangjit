// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "SimpleInterferenceGraphBuilder.h"

#include "ValueNumeration.h"

namespace Cangjie::JIT::X86_64 {

InterferenceGraph SimpleInterferenceGraphBuilder::Build(const CHIR::Function&, const ValueNumeration& numeration) const
{
    return BuildComplete(numeration.GetValueCount());
}

InterferenceGraph SimpleInterferenceGraphBuilder::BuildComplete(InterferenceGraph::NodeId nodeCount)
{
    InterferenceGraph graph(nodeCount);
    for (size_t firstIndex = 0; firstIndex < nodeCount; ++firstIndex) {
        const auto first = static_cast<InterferenceGraph::NodeId>(firstIndex);
        for (size_t secondIndex = firstIndex + 1; secondIndex < nodeCount; ++secondIndex) {
            graph.AddEdge(first, static_cast<InterferenceGraph::NodeId>(secondIndex));
        }
    }
    return graph;
}

} // namespace Cangjie::JIT::X86_64
