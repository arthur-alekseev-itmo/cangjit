// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "LivenessAnalysis.h"

#include <algorithm>
#include <cassert>

#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Value/Value.h"

namespace Cangjie::JIT::X86_64 {
namespace {
using Word = LivenessAnalysis::Word;
using Id = ValueNumeration::Id;
constexpr size_t BITS_PER_WORD = sizeof(Word) * 8;

Word* GetRow(std::vector<Word>& matrix, size_t row, size_t wordsPerRow) noexcept
{
    return wordsPerRow == 0 ? nullptr : matrix.data() + row * wordsPerRow;
}

void Add(Word* bitmap, Id id) noexcept
{
    bitmap[id / BITS_PER_WORD] |= Word{1} << (id % BITS_PER_WORD);
}

void Remove(Word* bitmap, Id id) noexcept
{
    bitmap[id / BITS_PER_WORD] &= ~(Word{1} << (id % BITS_PER_WORD));
}
} // namespace

LivenessAnalysis LivenessAnalysis::Analyze(const CHIR::Function& function, const ValueNumeration& numeration)
{
    LivenessAnalysis analysis;
    analysis.wordsPerBlock = (static_cast<size_t>(numeration.GetValueCount()) + BITS_PER_WORD - 1) / BITS_PER_WORD;
    analysis.CollectBlocks(function);
    analysis.Compute(numeration);
    return analysis;
}

bool LivenessAnalysis::Contains(const Word* bitmap, Id id) noexcept
{
    return (bitmap[id / BITS_PER_WORD] & (Word{1} << (id % BITS_PER_WORD))) != 0;
}

void LivenessAnalysis::Transfer(const CHIR::Expression& expression, const ValueNumeration& numeration, Word* live)
{
    if (const auto id = numeration.GetId(expression.GetResult())) {
        Remove(live, *id);
    }
    for (size_t i = 0; i < expression.GetNumOfNonSuccessorOperands(); ++i) {
        if (const auto id = numeration.GetId(expression.GetOperand(i))) {
            Add(live, *id);
        }
    }
}

std::optional<size_t> LivenessAnalysis::GetBlockIndex(const CHIR::Block* block) const noexcept
{
    const auto found = blockIndices.find(block);
    return found == blockIndices.end() ? std::nullopt : std::optional<size_t>(found->second);
}

const LivenessAnalysis::Word* LivenessAnalysis::GetLiveIn(size_t block) const noexcept
{
    return wordsPerBlock == 0 ? nullptr : liveIns.data() + block * wordsPerBlock;
}

const LivenessAnalysis::Word* LivenessAnalysis::GetLiveOut(size_t block) const noexcept
{
    return wordsPerBlock == 0 ? nullptr : liveOuts.data() + block * wordsPerBlock;
}

void LivenessAnalysis::CollectBlocks(const CHIR::Function& function)
{
    const auto* body = function.GetBody();
    assert(body != nullptr);
    blocks = body->GetAllBlocks();
    blocks.erase(std::remove_if(blocks.begin(), blocks.end(),
        [&](const CHIR::Block* block) { return block->GetFuncOrLambdaBody() != body; }), blocks.end());
    blockIndices.reserve(blocks.size());
    for (size_t i = 0; i < blocks.size(); ++i) {
        const bool inserted = blockIndices.emplace(blocks[i], i).second;
        assert(inserted);
        (void)inserted;
    }
}

void LivenessAnalysis::Compute(const ValueNumeration& numeration)
{
    std::vector<size_t> successorOffsets(blocks.size() + 1);
    std::vector<size_t> successorIndices;
    for (size_t i = 0; i < blocks.size(); ++i) {
        for (const auto* successor : blocks[i]->GetSuccessors()) {
            const auto found = blockIndices.find(successor);
            assert(found != blockIndices.end());
            if (found != blockIndices.end()) {
                successorIndices.push_back(found->second);
            }
        }
        successorOffsets[i + 1] = successorIndices.size();
    }
    const size_t matrixSize = blocks.size() * wordsPerBlock;
    std::vector<Word> uses(matrixSize), definitions(matrixSize);
    liveIns.resize(matrixSize);
    liveOuts.resize(matrixSize);
    for (size_t i = 0; i < blocks.size(); ++i) {
        auto* blockUses = GetRow(uses, i, wordsPerBlock);
        auto* blockDefinitions = GetRow(definitions, i, wordsPerBlock);
        for (const auto* expression : blocks[i]->GetExpressions()) {
            for (size_t operand = 0; operand < expression->GetNumOfNonSuccessorOperands(); ++operand) {
                const auto id = numeration.GetId(expression->GetOperand(operand));
                if (id && !Contains(blockDefinitions, *id)) {
                    Add(blockUses, *id);
                }
            }
            if (const auto id = numeration.GetId(expression->GetResult())) {
                Add(blockDefinitions, *id);
            }
        }
    }
    std::vector<Word> nextIn(wordsPerBlock), nextOut(wordsPerBlock);
    bool changed;
    do {
        changed = false;
        for (size_t reverse = blocks.size(); reverse > 0; --reverse) {
            const size_t block = reverse - 1;
            std::fill(nextOut.begin(), nextOut.end(), Word{0});
            for (size_t i = successorOffsets[block]; i < successorOffsets[block + 1]; ++i) {
                const auto* successor = GetLiveIn(successorIndices[i]);
                for (size_t word = 0; word < wordsPerBlock; ++word) {
                    nextOut[word] |= successor[word];
                }
            }
            const auto* blockUses = GetRow(uses, block, wordsPerBlock);
            const auto* blockDefinitions = GetRow(definitions, block, wordsPerBlock);
            auto* liveIn = GetRow(liveIns, block, wordsPerBlock);
            auto* liveOut = GetRow(liveOuts, block, wordsPerBlock);
            for (size_t word = 0; word < wordsPerBlock; ++word) {
                nextIn[word] = blockUses[word] | (nextOut[word] & ~blockDefinitions[word]);
                if (liveIn[word] != nextIn[word] || liveOut[word] != nextOut[word]) {
                    changed = true;
                    liveIn[word] = nextIn[word];
                    liveOut[word] = nextOut[word];
                }
            }
        }
    } while (changed);
}
} // namespace Cangjie::JIT::X86_64
