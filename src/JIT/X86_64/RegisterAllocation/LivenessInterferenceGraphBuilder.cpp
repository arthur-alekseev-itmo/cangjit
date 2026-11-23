// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "LivenessInterferenceGraphBuilder.h"

#include <algorithm>
#include <cassert>
#include <vector>

#include "LivenessAnalysis.h"
#include "ValueNumeration.h"
#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Value/Value.h"

namespace Cangjie::JIT::X86_64 {
namespace {
using Word = LivenessAnalysis::Word;
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
    const ValueNumeration& numeration, const LivenessAnalysis& liveness) const
{
    const auto nodeCount = numeration.GetValueCount();
    InterferenceGraph graph(nodeCount);
    const auto wordCount = liveness.GetWordCount();
    std::vector<Word> live(wordCount);
    const auto& blocks = liveness.GetBlocks();
    for (size_t block = 0; block < blocks.size(); ++block) {
        if (wordCount != 0) {
            const auto* liveOut = liveness.GetLiveOut(block);
            std::copy(liveOut, liveOut + wordCount, live.begin());
        }
        const auto expressions = blocks[block]->GetExpressions();
        for (auto expression = expressions.rbegin(); expression != expressions.rend(); ++expression) {
            if (const auto id = numeration.GetId((*expression)->GetResult())) {
                AddInterferences(graph, *id, live.data(), wordCount, nodeCount);
            }
            LivenessAnalysis::Transfer(**expression, numeration, live.data());
        }
    }
    const auto entry = liveness.GetBlockIndex(function.GetBody()->GetEntryBlock());
    assert(entry);
    if (entry) {
        AddClique(graph, liveness.GetLiveIn(*entry), wordCount, nodeCount);
    }
    return graph;
}
} // namespace Cangjie::JIT::X86_64
