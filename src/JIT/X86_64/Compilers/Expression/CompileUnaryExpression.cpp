// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "CompileUnaryExpression.h"

#include "CompileBinaryExpression.h"

#include "../../Utils/MemoryUtils.h"
#include "../../Utils/RegisterUtils.h"
#include "../Context/FunctionContext.h"
#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Type/Type.h"

#include <cstdint>

namespace Cangjie::JIT::X86_64 {
void CompileUnaryExpression::Compile(FunctionContext& ctx, const CHIR::UnaryExpressionBase& expr,
    const CHIR::Block* normal, const CHIR::Block* unwind)
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
        expr.GetOverflowStrategy() != OverflowStrategy::WRAPPING &&
        expr.GetOverflowStrategy() != OverflowStrategy::SATURATING &&
        expr.GetOverflowStrategy() != OverflowStrategy::THROWING) {
        ctx.Fail(JitFailureKind::UnsupportedUnaryOverflow);
        return;
    }
    if (!MemoryUtils::LoadValue(ctx, operand, asmjit::x86::rax)) {
        return;
    }
    auto& a = ctx.GetAssembler();
    switch (kind) {
        case K::NEG:
            if (expr.GetOverflowStrategy() == OverflowStrategy::THROWING) {
                RegisterUtils::NormalizeInteger(a, asmjit::x86::rax, width, type->IsSignedInteger());
                auto ordinary = a.newLabel();
                if (type->IsSignedInteger()) {
                    const uint64_t minimum = uint64_t{1} << (width - 1);
                    a.mov(asmjit::x86::rcx, width == 64 ? minimum : ~(minimum - 1));
                    a.cmp(asmjit::x86::rax, asmjit::x86::rcx);
                    a.jne(ordinary);
                } else {
                    a.test(asmjit::x86::rax, asmjit::x86::rax);
                    a.jz(ordinary);
                }
                EmitArithmeticException(ctx, expr, unwind, "CJ_JIT_Throw_Sub");
                a.bind(ordinary);
                a.neg(asmjit::x86::rax);
            } else if (expr.GetOverflowStrategy() == OverflowStrategy::SATURATING) {
                if (!type->IsSignedInteger()) {
                    a.xor_(asmjit::x86::rax, asmjit::x86::rax);
                } else {
                    RegisterUtils::NormalizeInteger(a, asmjit::x86::rax, width, true);
                    const uint64_t minimum = uint64_t{1} << (width - 1);
                    const uint64_t signExtendedMinimum = width == 64 ? minimum : ~(minimum - 1);
                    auto ordinary = a.newLabel();
                    auto done = a.newLabel();
                    a.mov(asmjit::x86::rcx, signExtendedMinimum);
                    a.cmp(asmjit::x86::rax, asmjit::x86::rcx);
                    a.jne(ordinary);
                    a.mov(asmjit::x86::rax, minimum - 1);
                    a.jmp(done);
                    a.bind(ordinary);
                    a.neg(asmjit::x86::rax);
                    a.bind(done);
                }
            } else {
                a.neg(asmjit::x86::rax);
            }
            break;
        case K::BITNOT: a.not_(asmjit::x86::rax); break;
        case K::NOT: a.xor_(asmjit::x86::rax, 1); break;
        default: ctx.Fail(JitFailureKind::UnsupportedUnaryOperation); return;
    }
    if (width) {
        RegisterUtils::NormalizeInteger(a, asmjit::x86::rax, width, type->IsSignedInteger());
    }
    MemoryUtils::StoreValue(ctx, result, asmjit::x86::rax);
    if (normal && !ctx.GetFailure()) {
        a.jmp(ctx.GetBlockLabel(normal));
    }
}
} // namespace Cangjie::JIT::X86_64
