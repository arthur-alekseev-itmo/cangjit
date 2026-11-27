// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef CANGJIE_JIT_X86_64_COMPILERS_COMPILE_UNARY_EXPRESSION_H
#define CANGJIE_JIT_X86_64_COMPILERS_COMPILE_UNARY_EXPRESSION_H

namespace Cangjie::CHIR {
class UnaryExpressionBase;
class Block;
}

namespace Cangjie::JIT::X86_64 {
class FunctionContext;

class CompileUnaryExpression final {
public:
    static void Compile(FunctionContext& ctx, const CHIR::UnaryExpressionBase& expr,
        const CHIR::Block* normal = nullptr, const CHIR::Block* unwind = nullptr);
};
} // namespace Cangjie::JIT::X86_64

#endif
