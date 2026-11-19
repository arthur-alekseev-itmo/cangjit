// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "InterferenceGraph.h"

#include <cassert>
#include <ostream>

namespace Cangjie::JIT::X86_64 {
namespace {
constexpr size_t BITS_PER_WORD = sizeof(NeighborBitmap::BitmapWord) * 8;

NeighborBitmap::BitmapWord GetBitMask(NeighborBitmap::NodeId node) noexcept
{
    return NeighborBitmap::BitmapWord{1} << (static_cast<size_t>(node) % BITS_PER_WORD);
}
} // namespace

NeighborBitmap::NeighborBitmap(
    const BitmapWord* bitmapWords, size_t bitmapWordCount, NodeId totalNodeCount) noexcept
    : words(bitmapWords), wordCount(bitmapWordCount), nodeCount(totalNodeCount)
{
}

bool NeighborBitmap::Contains(NodeId node) const noexcept
{
    assert(node < nodeCount);
    const size_t word = static_cast<size_t>(node) / BITS_PER_WORD;
    return (words[word] & GetBitMask(node)) != 0;
}

const NeighborBitmap::BitmapWord* NeighborBitmap::begin() const noexcept
{
    return words;
}

const NeighborBitmap::BitmapWord* NeighborBitmap::end() const noexcept
{
    return words + wordCount;
}

size_t NeighborBitmap::GetWordCount() const noexcept
{
    return wordCount;
}

InterferenceGraph::InterferenceGraph(NodeId totalNodeCount)
    : nodeCount(totalNodeCount),
      wordsPerNode((static_cast<size_t>(totalNodeCount) + BITS_PER_WORD - 1) / BITS_PER_WORD),
      adjacency(static_cast<size_t>(totalNodeCount) * wordsPerNode, BitmapWord{0})
{
}

InterferenceGraph::NodeId InterferenceGraph::GetNodeCount() const noexcept
{
    return nodeCount;
}

void InterferenceGraph::AddEdge(NodeId first, NodeId second) noexcept
{
    assert(first < nodeCount);
    assert(second < nodeCount);
    if (first == second) {
        return;
    }

    adjacency[GetWordIndex(first, second)] |= GetBitMask(second);
    adjacency[GetWordIndex(second, first)] |= GetBitMask(first);
}

bool InterferenceGraph::Interferes(NodeId first, NodeId second) const noexcept
{
    assert(first < nodeCount);
    assert(second < nodeCount);
    if (first == second) {
        return false;
    }
    return (adjacency[GetWordIndex(first, second)] & GetBitMask(second)) != 0;
}

NeighborBitmap InterferenceGraph::GetNeighbors(NodeId node) const noexcept
{
    assert(node < nodeCount);
    const auto rowOffset = static_cast<size_t>(node) * wordsPerNode;
    return NeighborBitmap(adjacency.data() + rowOffset, wordsPerNode, nodeCount);
}

void InterferenceGraph::Dump(std::ostream& output) const
{
    output << "InterferenceGraph(nodes=" << nodeCount << ")\n";
    for (size_t nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex) {
        const auto node = static_cast<NodeId>(nodeIndex);
        output << node << ':';
        for (size_t neighborIndex = 0; neighborIndex < nodeCount; ++neighborIndex) {
            const auto neighbor = static_cast<NodeId>(neighborIndex);
            if (Interferes(node, neighbor)) {
                output << ' ' << neighbor;
            }
        }
        output << '\n';
    }
}

size_t InterferenceGraph::GetWordIndex(NodeId row, NodeId column) const noexcept
{
    return static_cast<size_t>(row) * wordsPerNode + static_cast<size_t>(column) / BITS_PER_WORD;
}

} // namespace Cangjie::JIT::X86_64
