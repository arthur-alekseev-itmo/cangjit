// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "LivenessAnalysis.h"

#include <algorithm>
#include <utility>

#include "../JitAssert.h"
#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Value/Value.h"

namespace Cangjie::JIT::X86_64 {
namespace {
using Word = Liveness::Word;
using Id = ValueNumeration::Id;
constexpr size_t BITS_PER_WORD = sizeof(Word) * 8;

bool IsRuntimeExpression(const CHIR::Expression& expression)
{
    // JIT debug expressions emit no code and do not use their operands at runtime.
    return expression.GetExprKind() != CHIR::ExprKind::DEBUGEXPR;
}

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

Liveness LivenessAnalysis::Analyze(const CHIR::Function& function, const ValueNumeration& numeration)
{
    LivenessAnalysis analysis;
    analysis.wordsPerBlock = (static_cast<size_t>(numeration.GetValueCount()) + BITS_PER_WORD - 1) / BITS_PER_WORD;
    analysis.CollectBlocks(function);
    analysis.Compute(numeration);
    return analysis.MakeLiveness(numeration);
}


void LivenessAnalysis::Transfer(const CHIR::Expression& expression, const ValueNumeration& numeration, Word* live)
{
    if (!IsRuntimeExpression(expression)) {
        return;
    }
    if (const auto id = numeration.GetId(expression.GetResult())) {
        Remove(live, *id);
    }
    for (size_t i = 0; i < expression.GetNumOfNonSuccessorOperands(); ++i) {
        if (const auto id = numeration.GetId(expression.GetOperand(i))) {
            Add(live, *id);
        }
    }
}


void LivenessAnalysis::CollectBlocks(const CHIR::Function& function)
{
    auto body = function.GetBody();
    JIT_ASSERT(body != nullptr, "analyzed function has a body");
    blocks = body->GetAllBlocks();
    blocks.erase(std::remove_if(blocks.begin(), blocks.end(),
        [&](const CHIR::Block* block) { return block->GetFuncOrLambdaBody() != body; }), blocks.end());
    blockIndices.reserve(blocks.size());
    for (size_t i = 0; i < blocks.size(); ++i) {
        // GetAllBlocks lists each block of a body once.
        JIT_ASSERT(blockIndices.emplace(blocks[i], i).second, "block indices are unique");
    }
}

void LivenessAnalysis::Compute(const ValueNumeration& numeration)
{
    std::vector<size_t> successorOffsets(blocks.size() + 1);
    std::vector<size_t> successorIndices;
    for (size_t i = 0; i < blocks.size(); ++i) {
        for (auto successor : blocks[i]->GetSuccessors()) {
            const auto found = blockIndices.find(successor);
            JIT_ASSERT(found != blockIndices.end(), "CFG successor belongs to the analyzed function");
            successorIndices.push_back(found->second);
        }
        successorOffsets[i + 1] = successorIndices.size();
    }
    const size_t matrixSize = blocks.size() * wordsPerBlock;
    std::vector<Word> uses(matrixSize), definitions(matrixSize);
    liveIns.resize(matrixSize);
    liveOuts.resize(matrixSize);
    for (size_t i = 0; i < blocks.size(); ++i) {
        auto blockUses = GetRow(uses, i, wordsPerBlock);
        auto blockDefinitions = GetRow(definitions, i, wordsPerBlock);
        for (auto expression : blocks[i]->GetExpressions()) {
            if (!IsRuntimeExpression(*expression)) {
                continue;
            }
            for (size_t operand = 0; operand < expression->GetNumOfNonSuccessorOperands(); ++operand) {
                const auto id = numeration.GetId(expression->GetOperand(operand));
                if (id && !Liveness::Contains(blockDefinitions, *id)) {
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
                auto successor = GetRow(liveIns, successorIndices[i], wordsPerBlock);
                for (size_t word = 0; word < wordsPerBlock; ++word) {
                    nextOut[word] |= successor[word];
                }
            }
            auto blockUses = GetRow(uses, block, wordsPerBlock);
            auto blockDefinitions = GetRow(definitions, block, wordsPerBlock);
            auto liveIn = GetRow(liveIns, block, wordsPerBlock);
            auto liveOut = GetRow(liveOuts, block, wordsPerBlock);
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

Liveness LivenessAnalysis::MakeLiveness(const ValueNumeration& numeration)
{
    Liveness result;
    result.wordsPerSet = wordsPerBlock;
    result.blocks = std::move(blocks);
    result.blockIndices = std::move(blockIndices);
    result.blockOffsets.reserve(result.blocks.size());
    size_t rowCount = 0;
    for (auto block : result.blocks) {
        result.blockOffsets.push_back(rowCount);
        rowCount += block->GetExpressionsNum() + 1;
    }
    result.expressionIndices.reserve(rowCount - result.blocks.size());
    result.bitmaps.resize(rowCount * wordsPerBlock);
    for (size_t block = 0; block < result.blocks.size(); ++block) {
        auto chirBlock = result.blocks[block];
        const size_t count = chirBlock->GetExpressionsNum();
        const size_t first = result.blockOffsets[block];
        if (wordsPerBlock != 0) {
            auto liveOut = GetRow(liveOuts, block, wordsPerBlock);
            auto exit = GetRow(result.bitmaps, first + count, wordsPerBlock);
            std::copy(liveOut, liveOut + wordsPerBlock, exit);
        }
        for (size_t i = count; i > 0; --i) {
            auto expression = chirBlock->GetExpressionByIdx(i - 1);
            const size_t row = first + i - 1;
            // Each expression appears in exactly one block at one index.
            JIT_ASSERT(result.expressionIndices.emplace(expression, row).second,
                "expression rows are unique");
            auto before = GetRow(result.bitmaps, row, wordsPerBlock);
            if (wordsPerBlock != 0) {
                auto after = GetRow(result.bitmaps, row + 1, wordsPerBlock);
                std::copy(after, after + wordsPerBlock, before);
            }
            Transfer(*expression, numeration, before);
        }
    }
    return result;
}
} // namespace Cangjie::JIT::X86_64
