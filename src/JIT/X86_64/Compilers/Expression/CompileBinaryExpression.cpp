// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "CompileBinaryExpression.h"

#include "../../Utils/MemoryUtils.h"
#include "../../Utils/RegisterUtils.h"
#include "../Context/FunctionContext.h"
#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Type/Type.h"

namespace Cangjie::JIT::X86_64 {
void CompileBinaryExpression::Compile(FunctionContext& ctx, const CHIR::BinaryExpression& expr)
{
    using K = CHIR::BinaryExprKind;
    const auto* lhs = expr.GetLHSOperand();
    const auto* rhs = expr.GetRHSOperand();
    const auto* result = expr.GetResult();
    if (!lhs || !rhs || !result || !lhs->GetType() || !rhs->GetType() || !result->GetType()) {
        ctx.Fail(JitFailureKind::MissingBinaryOperandOrType);
        return;
    }
    const auto* type = lhs->GetType();
    const auto kind = expr.GetOpKind();
    const auto width = RegisterUtils::IntegerWidth(*type);
    const bool boolean = type->IsBoolean();
    const bool comparison = kind >= K::LT && kind <= K::NOTEQUAL;
    const bool arithmetic = kind == K::ADD || kind == K::SUB || kind == K::MUL;
    const bool bitwise = kind == K::BITAND || kind == K::BITOR || kind == K::BITXOR;
    const bool logical = kind == K::AND || kind == K::OR;
    if (type->GetTypeKind() != rhs->GetType()->GetTypeKind() ||
        (comparison || logical ? !result->GetType()->IsBoolean() :
            result->GetType()->GetTypeKind() != type->GetTypeKind())) {
        ctx.Fail(JitFailureKind::IncompatibleBinaryTypes);
        return;
    }
    if ((!width && !boolean) || (arithmetic && !width) || (bitwise && !width) ||
        (logical && !boolean) || (comparison && boolean && kind != K::EQUAL && kind != K::NOTEQUAL) ||
        !(arithmetic || bitwise || comparison || logical)) {
        ctx.Fail(JitFailureKind::UnsupportedBinaryOperation);
        return;
    }
    if (arithmetic && expr.GetOverflowStrategy() != OverflowStrategy::NA &&
        expr.GetOverflowStrategy() != OverflowStrategy::WRAPPING) {
        ctx.Fail(JitFailureKind::UnsupportedBinaryOverflow);
        return;
    }
    if (!MemoryUtils::LoadValue(ctx, lhs, asmjit::x86::rax) ||
        !MemoryUtils::LoadValue(ctx, rhs, asmjit::x86::rcx)) {
        return;
    }
    auto& a = ctx.GetAssembler();
    if (width) {
        RegisterUtils::NormalizeInteger(a, asmjit::x86::rax, width, type->IsSignedInteger());
        RegisterUtils::NormalizeInteger(a, asmjit::x86::rcx, width, type->IsSignedInteger());
    }
    switch (kind) {
        case K::ADD: a.add(asmjit::x86::rax, asmjit::x86::rcx); break;
        case K::SUB: a.sub(asmjit::x86::rax, asmjit::x86::rcx); break;
        case K::MUL: a.imul(asmjit::x86::rax, asmjit::x86::rcx); break;
        case K::BITAND: case K::AND: a.and_(asmjit::x86::rax, asmjit::x86::rcx); break;
        case K::BITOR: case K::OR: a.or_(asmjit::x86::rax, asmjit::x86::rcx); break;
        case K::BITXOR: a.xor_(asmjit::x86::rax, asmjit::x86::rcx); break;
        default:
            a.cmp(asmjit::x86::rax, asmjit::x86::rcx);
            switch (kind) {
                case K::EQUAL: a.sete(asmjit::x86::al); break;
                case K::NOTEQUAL: a.setne(asmjit::x86::al); break;
                case K::LT: if (type->IsSignedInteger()) a.setl(asmjit::x86::al); else a.setb(asmjit::x86::al); break;
                case K::LE: if (type->IsSignedInteger()) a.setle(asmjit::x86::al); else a.setbe(asmjit::x86::al); break;
                case K::GT: if (type->IsSignedInteger()) a.setg(asmjit::x86::al); else a.seta(asmjit::x86::al); break;
                case K::GE: if (type->IsSignedInteger()) a.setge(asmjit::x86::al); else a.setae(asmjit::x86::al); break;
                default: ctx.Fail(JitFailureKind::UnsupportedBinaryComparison); return;
            }
            a.movzx(asmjit::x86::rax, asmjit::x86::al);
            break;
    }
    if (width && !comparison) {
        RegisterUtils::NormalizeInteger(a, asmjit::x86::rax, width, type->IsSignedInteger());
    }
    MemoryUtils::StoreValue(ctx, result, asmjit::x86::rax);
}
} // namespace Cangjie::JIT::X86_64
