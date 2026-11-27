// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_REGISTER_ALLOCATION_INTERFERENCE_GRAPH_H
#define CANGJIE_JIT_X86_64_REGISTER_ALLOCATION_INTERFERENCE_GRAPH_H

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <vector>

namespace Cangjie::JIT::X86_64 {

class NeighborBitmap final {
public:
    using NodeId = uint32_t;
    using BitmapWord = uint64_t;

    NeighborBitmap(const BitmapWord* bitmapWords, size_t bitmapWordCount, NodeId totalNodeCount) noexcept;

    bool Contains(NodeId node) const noexcept;
    const BitmapWord* begin() const noexcept;
    const BitmapWord* end() const noexcept;
    size_t GetWordCount() const noexcept;

private:
    const BitmapWord* words;
    size_t wordCount;
    NodeId nodeCount;
};

class InterferenceGraph final {
public:
    using NodeId = NeighborBitmap::NodeId;
    using BitmapWord = NeighborBitmap::BitmapWord;

    explicit InterferenceGraph(NodeId totalNodeCount);

    NodeId GetNodeCount() const noexcept;
    void AddEdge(NodeId first, NodeId second) noexcept;
    bool Interferes(NodeId first, NodeId second) const noexcept;
    NeighborBitmap GetNeighbors(NodeId node) const noexcept;
    void Dump(std::ostream& output) const;

private:
    size_t GetWordIndex(NodeId row, NodeId column) const noexcept;

    NodeId nodeCount;
    size_t wordsPerNode;
    std::vector<BitmapWord> adjacency;
};

} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_REGISTER_ALLOCATION_INTERFERENCE_GRAPH_H
