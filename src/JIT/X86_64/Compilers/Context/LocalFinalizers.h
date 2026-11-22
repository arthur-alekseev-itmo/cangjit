// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_LOCAL_FINALIZERS_H
#define CANGJIE_JIT_X86_64_LOCAL_FINALIZERS_H

#include <asmjit/core.h>
#include <unordered_map>
#include <vector>

namespace Cangjie::CHIR { class Block; class Expression; class Value; }

namespace Cangjie::JIT::X86_64 {
class FunctionContext;

class LocalFinalizers final {
public:
    explicit LocalFinalizers(FunctionContext& context) noexcept : context(context) {}
    bool Prepare();
    void SetCurrentBlock(const CHIR::Block& block) noexcept;
    bool EmitAfter(const CHIR::Expression& expression, const CHIR::Expression& next);
    bool EmitEdges();
    asmjit::Label GetEdgeLabel(const CHIR::Block* successor);

private:
    struct Edge {
        const CHIR::Block* predecessor;
        const CHIR::Block* successor;
        asmjit::Label label;
        std::vector<const CHIR::Value*> objects;
    };
    bool EmitRemovals(const std::vector<const CHIR::Value*>& objects, const CHIR::Expression& next);

    FunctionContext& context;
    const CHIR::Block* currentBlock = nullptr;
    std::unordered_map<const CHIR::Expression*, std::vector<const CHIR::Value*>> finalizersAfter;
    std::vector<Edge> finalizerEdges;
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_LOCAL_FINALIZERS_H
