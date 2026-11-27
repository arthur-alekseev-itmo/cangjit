// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "CompileUnaryExpression.h"

#include "CompileBinaryExpression.h"

#include "../../Utils/MemoryUtils.h"
#include "../../Utils/TypeUtils.h"
#include "../../Utils/RegisterUtils.h"
#include "../../JitAssert.h"
#include "../Context/FunctionContext.h"
#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Type/Type.h"

#include <cstdint>

namespace Cangjie::JIT::X86_64 {
void CompileUnaryExpression::Compile(FunctionContext& ctx, const CHIR::UnaryExpressionBase& expr,
    const CHIR::Block* normal, const CHIR::Block* unwind)
{
    using K = CHIR::UnaryExprKind;
    auto operand = expr.GetOperand();
    auto result = expr.GetResult();
    JIT_ASSERT(operand && result && operand->GetType() && result->GetType() &&
        operand->GetType()->GetTypeKind() == result->GetType()->GetTypeKind(),
        "unary expression has matching operand and result types");
    auto type = operand->GetType();
    const auto width = TypeUtils::IntegerWidth(*type);
    const auto kind = expr.GetOpKind();
    const bool floating = TypeUtils::IsFloating(*type);
    if (floating && kind == K::NEG) {
        auto operandLocation = ctx.GetValueLocation(operand);
        MemoryUtils::Move(ctx, RegisterUtils::ToLocation(asmjit::x86::rax),
            *operandLocation, *type, asmjit::x86::r11);
        auto& a = ctx.GetAssembler();
        const uint64_t sign = type->GetTypeKind() == CHIR::Type::TYPE_FLOAT32
            ? uint64_t{1} << 31 : uint64_t{1} << 63;
        a.mov(asmjit::x86::rcx, asmjit::imm(sign));
        a.xor_(asmjit::x86::rax, asmjit::x86::rcx);
        auto resultLocation = ctx.GetValueLocation(result);
        MemoryUtils::Move(ctx, *resultLocation,
            RegisterUtils::ToLocation(asmjit::x86::rax), *result->GetType(), asmjit::x86::r11);
        if (normal) {
            a.jmp(ctx.GetBlockLabel(normal));
        }
        return;
    }
    JIT_ASSERT((kind != K::NOT || type->IsBoolean()) &&
        ((kind != K::NEG && kind != K::BITNOT) || width) &&
        (kind == K::NOT || kind == K::NEG || kind == K::BITNOT),
        "unary operation and operand type are supported");
    JIT_ASSERT(kind != K::NEG || expr.GetOverflowStrategy() == OverflowStrategy::NA ||
        expr.GetOverflowStrategy() == OverflowStrategy::WRAPPING ||
        expr.GetOverflowStrategy() == OverflowStrategy::SATURATING ||
        expr.GetOverflowStrategy() == OverflowStrategy::THROWING,
        "unary negation overflow strategy is supported");
    auto operandLocation = ctx.GetValueLocation(operand);
    MemoryUtils::Move(ctx, RegisterUtils::ToLocation(asmjit::x86::rax),
        *operandLocation, *type, asmjit::x86::r11);
    auto& a = ctx.GetAssembler();
    switch (kind) {
        case K::NEG:
            if (expr.GetOverflowStrategy() == OverflowStrategy::THROWING) {
                TypeUtils::NormalizeInteger(a, asmjit::x86::rax, width, type->IsSignedInteger());
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
                EmitArithmeticException(ctx, expr, unwind, "sub");
                a.bind(ordinary);
                a.neg(asmjit::x86::rax);
            } else if (expr.GetOverflowStrategy() == OverflowStrategy::SATURATING) {
                if (!type->IsSignedInteger()) {
                    a.xor_(asmjit::x86::rax, asmjit::x86::rax);
                } else {
                    TypeUtils::NormalizeInteger(a, asmjit::x86::rax, width, true);
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
        default: JIT_FAIL("unary operation kind is supported");
    }
    if (width) {
        TypeUtils::NormalizeInteger(a, asmjit::x86::rax, width, type->IsSignedInteger());
    }
    auto resultLocation = ctx.GetValueLocation(result);
    MemoryUtils::Move(ctx, *resultLocation, RegisterUtils::ToLocation(asmjit::x86::rax),
        *result->GetType(), asmjit::x86::r11);
    if (normal) {
        a.jmp(ctx.GetBlockLabel(normal));
    }
}
} // namespace Cangjie::JIT::X86_64
