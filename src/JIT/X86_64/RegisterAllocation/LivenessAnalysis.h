// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_REGISTER_ALLOCATION_LIVENESS_ANALYSIS_H
#define CANGJIE_JIT_X86_64_REGISTER_ALLOCATION_LIVENESS_ANALYSIS_H

#include <unordered_map>
#include <vector>

#include "Liveness.h"

namespace Cangjie::CHIR {
class Block;
class Expression;
class Function;
}

namespace Cangjie::JIT::X86_64 {
class LivenessAnalysis final {
public:
    static Liveness Analyze(const CHIR::Function& function, const ValueNumeration& numeration);

private:
    using Word = Liveness::Word;

    static void Transfer(const CHIR::Expression& expression, const ValueNumeration& numeration, Word* live);
    Liveness MakeLiveness(const ValueNumeration& numeration);
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
