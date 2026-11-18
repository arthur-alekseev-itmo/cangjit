// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef CANGJIE_JIT_X86_64_COMPILERS_COMPILE_BINARY_EXPRESSION_H
#define CANGJIE_JIT_X86_64_COMPILERS_COMPILE_BINARY_EXPRESSION_H

namespace Cangjie::CHIR {
class BinaryExpression;
}

namespace Cangjie::JIT::X86_64 {
class FunctionContext;

class CompileBinaryExpression final {
public:
    static void Compile(FunctionContext& ctx, const CHIR::BinaryExpression& expr);
};
} // namespace Cangjie::JIT::X86_64

#endif
