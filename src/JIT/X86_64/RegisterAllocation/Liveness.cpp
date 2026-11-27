// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Liveness.h"

#include <cassert>
#include <ostream>

#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Value/Value.h"

namespace Cangjie::JIT::X86_64 {
namespace {
constexpr size_t BITS_PER_WORD = sizeof(Liveness::Word) * 8;
}

bool Liveness::Contains(const Word* bitmap, ValueNumeration::Id id) noexcept
{
    return (bitmap[id / BITS_PER_WORD] & (Word{1} << (id % BITS_PER_WORD))) != 0;
}

bool Liveness::ContainsExpression(const CHIR::Expression& expression) const noexcept
{
    return expressionIndices.find(&expression) != expressionIndices.end();
}

const Liveness::Word* Liveness::GetLiveBefore(const CHIR::Expression& expression) const noexcept
{
    const auto found = expressionIndices.find(&expression);
    assert(found != expressionIndices.end());
    return wordsPerSet == 0 ? nullptr : bitmaps.data() + found->second * wordsPerSet;
}

const Liveness::Word* Liveness::GetLiveAfter(const CHIR::Expression& expression) const noexcept
{
    const auto found = expressionIndices.find(&expression);
    assert(found != expressionIndices.end());
    return wordsPerSet == 0 ? nullptr : bitmaps.data() + (found->second + 1) * wordsPerSet;
}

const Liveness::Word* Liveness::GetLiveIn(const CHIR::Block* block) const noexcept
{
    const auto index = GetBlockIndex(block);
    assert(index);
    return wordsPerSet == 0 ? nullptr : bitmaps.data() + blockOffsets[*index] * wordsPerSet;
}

std::optional<size_t> Liveness::GetBlockIndex(const CHIR::Block* block) const noexcept
{
    const auto found = blockIndices.find(block);
    return found == blockIndices.end() ? std::nullopt : std::optional<size_t>(found->second);
}

void Liveness::Dump(std::ostream& output, const ValueNumeration& numeration) const
{
    for (auto block : blocks) {
        output << "  block " << block->GetIdentifier() << ":\n";
        for (size_t i = 0; i < block->GetExpressionsNum(); ++i) {
            auto expression = block->GetExpressionByIdx(i);
            auto live = GetLiveBefore(*expression);
            output << "    before " << expression->ToString(0) << " : {";
            bool first = true;
            for (ValueNumeration::Id id = 0; id < numeration.GetValueCount(); ++id) {
                if (!Contains(live, id)) {
                    continue;
                }
                if (!first) {
                    output << ", ";
                }
                output << numeration.GetValue(id)->GetIdentifier();
                first = false;
            }
            output << "}\n";
        }
    }
}
} // namespace Cangjie::JIT::X86_64
