// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "CompileUnaryExpression.h"

#include "../Utils/MemoryUtils.h"
#include "../Utils/RegisterUtils.h"
#include "Context/FunctionContext.h"
#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Type/Type.h"

namespace Cangjie::JIT::X86_64 {
void CompileUnaryExpression::Compile(FunctionContext& ctx, const CHIR::UnaryExpression& expr)
{
    using K = CHIR::UnaryExprKind;
    const auto* operand = expr.GetOperand();
    const auto* result = expr.GetResult();
    if (!operand || !result || !operand->GetType() || !result->GetType() ||
        operand->GetType()->GetTypeKind() != result->GetType()->GetTypeKind()) {
        ctx.Fail(JitFailureKind::InvalidUnaryOperandOrType);
        return;
    }
    const auto* type = operand->GetType();
    const auto width = RegisterUtils::IntegerWidth(*type);
    const auto kind = expr.GetOpKind();
    if ((kind == K::NOT && !type->IsBoolean()) ||
        ((kind == K::NEG || kind == K::BITNOT) && !width) ||
        (kind != K::NOT && kind != K::NEG && kind != K::BITNOT)) {
        ctx.Fail(JitFailureKind::UnsupportedUnaryOperationOrType);
        return;
    }
    if (kind == K::NEG && expr.GetOverflowStrategy() != OverflowStrategy::NA &&
        expr.GetOverflowStrategy() != OverflowStrategy::WRAPPING) {
        ctx.Fail(JitFailureKind::UnsupportedUnaryOverflow);
        return;
    }
    if (!MemoryUtils::LoadValue(ctx, operand, asmjit::x86::rax)) {
        return;
    }
    auto& a = ctx.GetAssembler();
    switch (kind) {
        case K::NEG: a.neg(asmjit::x86::rax); break;
        case K::BITNOT: a.not_(asmjit::x86::rax); break;
        case K::NOT: a.xor_(asmjit::x86::rax, 1); break;
        default: ctx.Fail(JitFailureKind::UnsupportedUnaryOperation); return;
    }
    if (width) {
        RegisterUtils::NormalizeInteger(a, asmjit::x86::rax, width, type->IsSignedInteger());
    }
    MemoryUtils::StoreValue(ctx, result, asmjit::x86::rax);
}
} // namespace Cangjie::JIT::X86_64
