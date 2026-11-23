// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_REGISTER_ALLOCATION_LIVENESS_ANALYSIS_H
#define CANGJIE_JIT_X86_64_REGISTER_ALLOCATION_LIVENESS_ANALYSIS_H

#include <iosfwd>
#include <optional>
#include <unordered_map>
#include <vector>

#include "InterferenceGraph.h"
#include "ValueNumeration.h"

namespace Cangjie::CHIR {
class Block;
class Expression;
class Function;
}

namespace Cangjie::JIT::X86_64 {
class LivenessAnalysis final {
public:
    using Word = InterferenceGraph::BitmapWord;

    static LivenessAnalysis Analyze(const CHIR::Function& function, const ValueNumeration& numeration);
    static bool Contains(const Word* bitmap, ValueNumeration::Id id) noexcept;
    static void Transfer(const CHIR::Expression& expression, const ValueNumeration& numeration, Word* live);

    const std::vector<CHIR::Block*>& GetBlocks() const noexcept { return blocks; }
    std::optional<size_t> GetBlockIndex(const CHIR::Block* block) const noexcept;
    size_t GetWordCount() const noexcept { return wordsPerBlock; }
    const Word* GetLiveIn(size_t block) const noexcept;
    const Word* GetLiveOut(size_t block) const noexcept;
    void Dump(std::ostream& output, const ValueNumeration& numeration) const;

private:
    void CollectBlocks(const CHIR::Function& function);
    void Compute(const ValueNumeration& numeration);

    size_t wordsPerBlock = 0;
    std::vector<CHIR::Block*> blocks;
    std::unordered_map<const CHIR::Block*, size_t> blockIndices;
    std::vector<Word> liveIns;
    std::vector<Word> liveOuts;
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_REGISTER_ALLOCATION_LIVENESS_ANALYSIS_H
