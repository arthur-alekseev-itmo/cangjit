// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_REGISTER_ALLOCATION_LIVENESS_H
#define CANGJIE_JIT_X86_64_REGISTER_ALLOCATION_LIVENESS_H

#include <iosfwd>
#include <unordered_map>
#include <vector>

#include "InterferenceGraph.h"
#include "ValueNumeration.h"

namespace Cangjie::CHIR {
class Block;
class Expression;
}

namespace Cangjie::JIT::X86_64 {
class LivenessAnalysis;

class Liveness final {
public:
    using Word = InterferenceGraph::BitmapWord;

    static bool Contains(const Word* bitmap, ValueNumeration::Id id) noexcept;
    bool ContainsExpression(const CHIR::Expression& expression) const noexcept;
    const Word* GetLiveBefore(const CHIR::Expression& expression) const noexcept;
    const Word* GetLiveAfter(const CHIR::Expression& expression) const noexcept;
    const Word* GetLiveIn(const CHIR::Block* block) const noexcept;
    size_t GetBlockIndex(const CHIR::Block* block) const noexcept;
    size_t GetWordCount() const noexcept { return wordsPerSet; }
    const std::vector<CHIR::Block*>& GetBlocks() const noexcept { return blocks; }
    void Dump(std::ostream& output, const ValueNumeration& numeration) const;

private:
    friend class LivenessAnalysis;

    size_t wordsPerSet = 0;
    std::vector<CHIR::Block*> blocks;
    std::unordered_map<const CHIR::Block*, size_t> blockIndices;
    std::unordered_map<const CHIR::Expression*, size_t> expressionIndices;
    std::vector<size_t> blockOffsets;
    std::vector<Word> bitmaps;
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_REGISTER_ALLOCATION_LIVENESS_H
