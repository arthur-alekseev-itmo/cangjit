// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef CANGJIE_JIT_X86_64_COMPILERS_COMPILE_BINARY_EXPRESSION_H
#define CANGJIE_JIT_X86_64_COMPILERS_COMPILE_BINARY_EXPRESSION_H

namespace Cangjie::CHIR {
class BinaryExpressionBase;
class Expression;
class Block;
}

namespace Cangjie::JIT::X86_64 {
class FunctionContext;

class CompileBinaryExpression final {
public:
    static void Compile(FunctionContext& ctx, const CHIR::BinaryExpressionBase& expr,
        const CHIR::Block* normal = nullptr, const CHIR::Block* unwind = nullptr);
};

// Emits a real runtime exception and records all call sites that can unwind.
void EmitArithmeticException(FunctionContext& ctx, const CHIR::Expression& expr,
    const CHIR::Block* unwind, const char* symbol);
} // namespace Cangjie::JIT::X86_64

#endif
