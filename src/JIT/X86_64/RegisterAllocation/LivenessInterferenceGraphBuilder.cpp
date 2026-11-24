// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "LivenessInterferenceGraphBuilder.h"

#include "Liveness.h"
#include "ValueNumeration.h"
#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Value/Value.h"
#include "cangjie/Utils/CheckUtils.h"

namespace Cangjie::JIT::X86_64 {
namespace {
using Word = Liveness::Word;
using NodeId = InterferenceGraph::NodeId;
constexpr size_t BITS_PER_WORD = sizeof(Word) * 8;

template <typename Callback>
void ForEachSetBit(const Word* bitmap, size_t wordCount, NodeId nodeCount, Callback&& callback)
{
    for (size_t wordIndex = 0; wordIndex < wordCount; ++wordIndex) {
        Word remaining = bitmap[wordIndex];
        while (remaining != 0) {
            const auto bit = static_cast<size_t>(__builtin_ctzll(static_cast<unsigned long long>(remaining)));
            const size_t index = wordIndex * BITS_PER_WORD + bit;
            if (index < nodeCount) {
                callback(static_cast<NodeId>(index));
            }
            remaining &= remaining - 1;
        }
    }
}

void AddInterferences(InterferenceGraph& graph, NodeId definition, const Word* live, size_t wordCount, NodeId nodeCount)
{
    ForEachSetBit(live, wordCount, nodeCount, [&](NodeId other) { graph.AddEdge(definition, other); });
}

void AddClique(InterferenceGraph& graph, const Word* values, size_t wordCount, NodeId nodeCount)
{
    ForEachSetBit(values, wordCount, nodeCount, [&](NodeId first) {
        ForEachSetBit(values, wordCount, nodeCount, [&](NodeId second) {
            if (first < second) {
                graph.AddEdge(first, second);
            }
        });
    });
}
} // namespace

InterferenceGraph LivenessInterferenceGraphBuilder::Build(const CHIR::Function& function,
    const ValueNumeration& numeration, const Liveness& liveness) const
{
    const auto nodeCount = numeration.GetValueCount();
    InterferenceGraph graph(nodeCount);
    const auto wordCount = liveness.GetWordCount();
    for (const auto* block : liveness.GetBlocks()) {
        for (size_t i = 0; i < block->GetExpressionsNum(); ++i) {
            const auto* expression = block->GetExpressionByIdx(i);
            if (const auto id = numeration.GetId(expression->GetResult())) {
                AddInterferences(graph, *id, liveness.GetLiveAfter(*expression), wordCount, nodeCount);
            }
        }
    }
    const auto entry = liveness.GetBlockIndex(function.GetBody()->GetEntryBlock());
    CJC_ASSERT(entry.has_value());
    if (entry) {
        AddClique(graph, liveness.GetLiveIn(function.GetBody()->GetEntryBlock()), wordCount, nodeCount);
    }
    return graph;
}
} // namespace Cangjie::JIT::X86_64
