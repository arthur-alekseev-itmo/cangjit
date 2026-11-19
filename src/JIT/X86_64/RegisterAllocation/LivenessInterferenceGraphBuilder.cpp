// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "LivenessInterferenceGraphBuilder.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <optional>
#include <unordered_map>
#include <vector>

#include "ValueNumeration.h"
#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Value/Value.h"

namespace Cangjie::JIT::X86_64 {
namespace {

using BitmapWord = InterferenceGraph::BitmapWord;
using NodeId = InterferenceGraph::NodeId;

constexpr size_t BITS_PER_WORD = sizeof(BitmapWord) * 8;

size_t GetWordCount(NodeId nodeCount) noexcept
{
    return (static_cast<size_t>(nodeCount) + BITS_PER_WORD - 1) / BITS_PER_WORD;
}

BitmapWord* GetRow(std::vector<BitmapWord>& matrix, size_t row, size_t wordsPerRow) noexcept
{
    return wordsPerRow == 0 ? nullptr : matrix.data() + row * wordsPerRow;
}

BitmapWord GetBitMask(NodeId node) noexcept
{
    return BitmapWord{1} << (static_cast<size_t>(node) % BITS_PER_WORD);
}

bool Contains(const BitmapWord* bitmap, NodeId node) noexcept
{
    return (bitmap[static_cast<size_t>(node) / BITS_PER_WORD] & GetBitMask(node)) != 0;
}

void Add(BitmapWord* bitmap, NodeId node) noexcept
{
    bitmap[static_cast<size_t>(node) / BITS_PER_WORD] |= GetBitMask(node);
}

void Remove(BitmapWord* bitmap, NodeId node) noexcept
{
    bitmap[static_cast<size_t>(node) / BITS_PER_WORD] &= ~GetBitMask(node);
}

template <typename Callback>
void ForEachSetBit(const BitmapWord* bitmap, size_t wordCount, NodeId nodeCount, Callback&& callback)
{
    for (size_t wordIndex = 0; wordIndex < wordCount; ++wordIndex) {
        BitmapWord remaining = bitmap[wordIndex];
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

void AddInterferences(
    InterferenceGraph& graph, NodeId definition, const BitmapWord* live, size_t wordCount, NodeId nodeCount)
{
    ForEachSetBit(live, wordCount, nodeCount, [&](NodeId other) { graph.AddEdge(definition, other); });
}

void AddClique(InterferenceGraph& graph, const BitmapWord* values, size_t wordCount, NodeId nodeCount)
{
    ForEachSetBit(values, wordCount, nodeCount, [&](NodeId first) {
        ForEachSetBit(values, wordCount, nodeCount, [&](NodeId second) {
            if (first < second) {
                graph.AddEdge(first, second);
            }
        });
    });
}

std::optional<ValueNumeration::Id> GetId(const ValueNumeration& numeration, const CHIR::Value* value) noexcept
{
    return value == nullptr ? std::nullopt : numeration.GetId(value);
}

} // namespace

InterferenceGraph LivenessInterferenceGraphBuilder::Build(
    const CHIR::Function& function, const ValueNumeration& numeration) const
{
    const auto nodeCount = numeration.GetValueCount();
    InterferenceGraph graph(nodeCount);
    const auto* body = function.GetBody();
    assert(body != nullptr);

    auto blocks = body->GetAllBlocks();
    blocks.erase(std::remove_if(blocks.begin(), blocks.end(),
                     [&](const CHIR::Block* block) { return block->GetFuncOrLambdaBody() != body; }),
        blocks.end());

    const size_t blockCount = blocks.size();
    const size_t wordsPerBlock = GetWordCount(nodeCount);
    const size_t matrixSize = blockCount * wordsPerBlock;
    std::vector<BitmapWord> uses(matrixSize, BitmapWord{0});
    std::vector<BitmapWord> definitions(matrixSize, BitmapWord{0});
    std::vector<BitmapWord> liveIns(matrixSize, BitmapWord{0});
    std::vector<BitmapWord> liveOuts(matrixSize, BitmapWord{0});

    std::unordered_map<const CHIR::Block*, size_t> blockIndices;
    blockIndices.reserve(blockCount);
    for (size_t blockIndex = 0; blockIndex < blockCount; ++blockIndex) {
        const bool inserted = blockIndices.emplace(blocks[blockIndex], blockIndex).second;
        assert(inserted);
        (void)inserted;
    }

    std::vector<size_t> successorOffsets(blockCount + 1, 0);
    std::vector<size_t> successorIndices;
    for (size_t blockIndex = 0; blockIndex < blockCount; ++blockIndex) {
        for (const auto* successor : blocks[blockIndex]->GetSuccessors()) {
            const auto found = blockIndices.find(successor);
            assert(found != blockIndices.end());
            if (found != blockIndices.end()) {
                successorIndices.push_back(found->second);
            }
        }
        successorOffsets[blockIndex + 1] = successorIndices.size();
    }

    for (size_t blockIndex = 0; blockIndex < blockCount; ++blockIndex) {
        auto* blockUses = GetRow(uses, blockIndex, wordsPerBlock);
        auto* blockDefinitions = GetRow(definitions, blockIndex, wordsPerBlock);
        for (const auto* expression : blocks[blockIndex]->GetExpressions()) {
            const size_t operandCount = expression->GetNumOfNonSuccessorOperands();
            for (size_t operandIndex = 0; operandIndex < operandCount; ++operandIndex) {
                const auto id = GetId(numeration, expression->GetOperand(operandIndex));
                if (id && !Contains(blockDefinitions, *id)) {
                    Add(blockUses, *id);
                }
            }
            if (const auto id = GetId(numeration, expression->GetResult())) {
                Add(blockDefinitions, *id);
            }
        }
    }

    std::vector<BitmapWord> nextLiveIn(wordsPerBlock, BitmapWord{0});
    std::vector<BitmapWord> nextLiveOut(wordsPerBlock, BitmapWord{0});
    bool changed;
    do {
        changed = false;
        for (size_t reverseIndex = blockCount; reverseIndex > 0; --reverseIndex) {
            const size_t blockIndex = reverseIndex - 1;
            std::fill(nextLiveOut.begin(), nextLiveOut.end(), BitmapWord{0});
            for (size_t successorIndex = successorOffsets[blockIndex];
                successorIndex < successorOffsets[blockIndex + 1]; ++successorIndex) {
                const auto* successorLiveIn = GetRow(liveIns, successorIndices[successorIndex], wordsPerBlock);
                for (size_t word = 0; word < wordsPerBlock; ++word) {
                    nextLiveOut[word] |= successorLiveIn[word];
                }
            }

            const auto* blockUses = GetRow(uses, blockIndex, wordsPerBlock);
            const auto* blockDefinitions = GetRow(definitions, blockIndex, wordsPerBlock);
            auto* blockLiveIn = GetRow(liveIns, blockIndex, wordsPerBlock);
            auto* blockLiveOut = GetRow(liveOuts, blockIndex, wordsPerBlock);
            for (size_t word = 0; word < wordsPerBlock; ++word) {
                nextLiveIn[word] = blockUses[word] | (nextLiveOut[word] & ~blockDefinitions[word]);
                if (blockLiveIn[word] != nextLiveIn[word] || blockLiveOut[word] != nextLiveOut[word]) {
                    changed = true;
                    blockLiveIn[word] = nextLiveIn[word];
                    blockLiveOut[word] = nextLiveOut[word];
                }
            }
        }
    } while (changed);

    std::vector<BitmapWord> live(wordsPerBlock, BitmapWord{0});
    for (size_t blockIndex = 0; blockIndex < blockCount; ++blockIndex) {
        const auto* blockLiveOut = GetRow(liveOuts, blockIndex, wordsPerBlock);
        if (wordsPerBlock != 0) {
            std::copy(blockLiveOut, blockLiveOut + wordsPerBlock, live.begin());
        }
        const auto expressions = blocks[blockIndex]->GetExpressions();
        for (auto expression = expressions.rbegin(); expression != expressions.rend(); ++expression) {
            if (const auto id = GetId(numeration, (*expression)->GetResult())) {
                AddInterferences(graph, *id, live.data(), wordsPerBlock, nodeCount);
                Remove(live.data(), *id);
            }
            const size_t operandCount = (*expression)->GetNumOfNonSuccessorOperands();
            for (size_t operandIndex = 0; operandIndex < operandCount; ++operandIndex) {
                if (const auto id = GetId(numeration, (*expression)->GetOperand(operandIndex))) {
                    Add(live.data(), *id);
                }
            }
        }
    }

    const auto entry = blockIndices.find(body->GetEntryBlock());
    assert(entry != blockIndices.end());
    if (entry != blockIndices.end()) {
        AddClique(graph, GetRow(liveIns, entry->second, wordsPerBlock), wordsPerBlock, nodeCount);
    }
    return graph;
}

} // namespace Cangjie::JIT::X86_64
