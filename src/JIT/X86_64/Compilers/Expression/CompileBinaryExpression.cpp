// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "CompileBinaryExpression.h"

#include "../../Utils/MemoryUtils.h"
#include "../../Utils/RegisterUtils.h"
#include "../Context/GlobalContext.h"
#include "../Context/FunctionContext.h"
#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Type/Type.h"

#include <cstdint>

namespace {
using namespace asmjit;
using namespace asmjit::x86;

// Operands are normalized to their declared width. Preserve their signs to
// select the appropriate endpoint if the exact result is out of range.
void CheckedArithmetic(Cangjie::JIT::X86_64::FunctionContext& ctx,
    const Cangjie::CHIR::BinaryExpressionBase& expr, const Cangjie::CHIR::Block* unwind,
    Cangjie::CHIR::BinaryExprKind kind, unsigned width, bool isSigned)
{
    using K = Cangjie::CHIR::BinaryExprKind;
    auto& a = ctx.GetAssembler();
    auto overflow = a.newLabel();
    auto done = a.newLabel();
    a.mov(r8, rax);
    if (!isSigned && width == 64 && kind == K::MUL) {
        a.mul(rcx);
        a.test(rdx, rdx);
        a.jnz(overflow);
    } else {
        if (kind == K::ADD) {
            a.add(rax, rcx);
        } else if (kind == K::SUB) {
            a.sub(rax, rcx);
        } else {
            a.imul(rax, rcx);
        }
        if (width == 64) {
            if (isSigned) {
                a.jo(overflow);
            } else {
                a.jc(overflow);
            }
        } else {
            a.mov(rdx, rax);
            Cangjie::JIT::X86_64::RegisterUtils::NormalizeInteger(a, rdx, width, isSigned);
            a.cmp(rax, rdx);
            a.jne(overflow);
        }
    }
    a.jmp(done);
    a.bind(overflow);
    if (expr.GetOverflowStrategy() == Cangjie::OverflowStrategy::THROWING) {
        const char* message = kind == K::ADD ? "add" : kind == K::SUB ? "sub" : "mul";
        Cangjie::JIT::X86_64::EmitArithmeticException(ctx, expr, unwind, message);
        a.bind(done);
        return;
    }
    if (!isSigned) {
        const uint64_t maximum = width == 64 ? UINT64_MAX : (uint64_t{1} << width) - 1;
        a.mov(rax, kind == K::SUB ? uint64_t{0} : maximum);
    } else {
        const uint64_t minimum = uint64_t{1} << (width - 1);
        const uint64_t maximum = minimum - 1;
        auto positive = a.newLabel();
        if (kind == K::MUL) {
            a.xor_(r8, rcx);
        }
        a.test(r8, r8);
        a.jns(positive);
        a.mov(rax, minimum);
        a.jmp(done);
        a.bind(positive);
        a.mov(rax, maximum);
    }
    a.bind(done);
}
} // namespace

namespace Cangjie::JIT::X86_64 {
void EmitArithmeticException(FunctionContext& ctx, const CHIR::Expression& expr,
    const CHIR::Block* unwind, const char* message, bool overflow)
{
    auto& global = ctx.GetGlobalContext();
    const auto* literal = global.GetStringLiteral(message);
    auto factory = global.GetRuntimeBindings().Resolve(
        overflow ? "rt$CreateOverflowException_msg" : "rt$CreateArithmeticException_msg");
    auto thrower = global.GetRuntimeBindings().Resolve("CJ_MCC_ThrowException");
    auto* factoryAddress = std::get_if<void*>(&factory);
    auto* throwAddress = std::get_if<void*>(&thrower);
    if (!literal || !factoryAddress || !*factoryAddress || !throwAddress || !*throwAddress ||
        !expr.GetResult()) {
        ctx.Fail(JitFailureKind::UnresolvedSymbol, expr.GetExprKind());
        return;
    }
    auto& a = ctx.GetAssembler();
    a.long_().mov(asmjit::x86::rdi, asmjit::imm(reinterpret_cast<uintptr_t>(literal)));
    a.long_().mov(asmjit::x86::r11, asmjit::imm(reinterpret_cast<uintptr_t>(*factoryAddress)));
    const auto factoryStart = a.offset();
    a.call(asmjit::x86::r11);
    const auto factoryEnd = a.offset();
    if (!ctx.RecordSafepoint(expr) || !MemoryUtils::StoreValue(ctx, expr.GetResult(), asmjit::x86::rax)) {
        return;
    }
    a.mov(asmjit::x86::rdi, asmjit::x86::rax);
    a.long_().mov(asmjit::x86::r11, asmjit::imm(reinterpret_cast<uintptr_t>(*throwAddress)));
    const auto throwStart = a.offset();
    a.call(asmjit::x86::r11);
    const auto throwEnd = a.offset();
    if (!ctx.RecordSafepoint(expr, expr.GetResult())) {
        return;
    }
    a.ud2();
    if (unwind) {
        ctx.EmitExceptionLanding(*unwind, factoryStart, factoryEnd);
        ctx.EmitExceptionLanding(*unwind, throwStart, throwEnd);
    }
}

void CompileBinaryExpression::Compile(FunctionContext& ctx, const CHIR::BinaryExpressionBase& expr,
    const CHIR::Block* normal, const CHIR::Block* unwind)
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
    const auto rhsWidth = RegisterUtils::IntegerWidth(*rhs->GetType());
    const bool shift = kind == K::LSHIFT || kind == K::RSHIFT;
    const bool boolean = type->IsBoolean();
    const bool comparison = kind >= K::LT && kind <= K::NOTEQUAL;
    const bool arithmetic = kind == K::ADD || kind == K::SUB || kind == K::MUL ||
        kind == K::DIV || kind == K::MOD;
    const bool bitwise = kind == K::BITAND || kind == K::BITOR || kind == K::BITXOR;
    const bool logical = kind == K::AND || kind == K::OR;
    if ((!shift && type->GetTypeKind() != rhs->GetType()->GetTypeKind()) ||
        (comparison || logical ? !result->GetType()->IsBoolean() :
            result->GetType()->GetTypeKind() != type->GetTypeKind())) {
        ctx.Fail(JitFailureKind::IncompatibleBinaryTypes);
        return;
    }
    if ((!width && !boolean) || (arithmetic && !width) || (bitwise && !width) ||
        (shift && (!width || !rhsWidth)) || (logical && !boolean) ||
        (comparison && boolean && kind != K::EQUAL && kind != K::NOTEQUAL) ||
        !(arithmetic || bitwise || comparison || logical || shift)) {
        ctx.Fail(JitFailureKind::UnsupportedBinaryOperation);
        return;
    }
    if (arithmetic && expr.GetOverflowStrategy() != OverflowStrategy::NA &&
        expr.GetOverflowStrategy() != OverflowStrategy::WRAPPING &&
        expr.GetOverflowStrategy() != OverflowStrategy::SATURATING &&
        expr.GetOverflowStrategy() != OverflowStrategy::THROWING) {
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
        RegisterUtils::NormalizeInteger(a, asmjit::x86::rcx, shift ? rhsWidth : width,
            shift || type->IsSignedInteger());
    }
    switch (kind) {
        case K::ADD: case K::SUB: case K::MUL:
            if (expr.GetOverflowStrategy() == OverflowStrategy::SATURATING ||
                expr.GetOverflowStrategy() == OverflowStrategy::THROWING) {
                CheckedArithmetic(ctx, expr, unwind, kind, width, type->IsSignedInteger());
            } else if (kind == K::ADD) {
                a.add(asmjit::x86::rax, asmjit::x86::rcx);
            } else if (kind == K::SUB) {
                a.sub(asmjit::x86::rax, asmjit::x86::rcx);
            } else {
                a.imul(asmjit::x86::rax, asmjit::x86::rcx);
            }
            break;
        case K::LSHIFT: case K::RSHIFT: {
            auto nonnegative = a.newLabel();
            auto inRange = a.newLabel();
            a.test(asmjit::x86::rcx, asmjit::x86::rcx);
            a.jns(nonnegative);
            EmitArithmeticException(ctx, expr, unwind, "Overshift: Negative shift count!", false);
            a.bind(nonnegative);
            a.cmp(asmjit::x86::rcx, width);
            a.jl(inRange);
            EmitArithmeticException(ctx, expr, unwind,
                "Overshift: Value of right operand is greater than or equal to the width of left operand!", false);
            a.bind(inRange);
            if (kind == K::LSHIFT) {
                a.shl(asmjit::x86::rax, asmjit::x86::cl);
            } else if (type->IsSignedInteger()) {
                a.sar(asmjit::x86::rax, asmjit::x86::cl);
            } else {
                a.shr(asmjit::x86::rax, asmjit::x86::cl);
            }
            break;
        }
        case K::DIV: case K::MOD: {
            auto nonzero = a.newLabel();
            auto divide = a.newLabel();
            auto done = a.newLabel();
            a.test(asmjit::x86::rcx, asmjit::x86::rcx);
            a.jnz(nonzero);
            EmitArithmeticException(ctx, expr, unwind, "Divided by zero!", false);
            a.bind(nonzero);
            if (type->IsSignedInteger()) {
                const uint64_t minimum = uint64_t{1} << (width - 1);
                a.mov(asmjit::x86::rdx, width == 64 ? minimum : ~(minimum - 1));
                a.cmp(asmjit::x86::rax, asmjit::x86::rdx);
                a.jne(divide);
                a.cmp(asmjit::x86::rcx, -1);
                a.jne(divide);
                if (kind == K::MOD) {
                    a.xor_(asmjit::x86::rax, asmjit::x86::rax);
                } else if (expr.GetOverflowStrategy() == OverflowStrategy::SATURATING ||
                    expr.GetOverflowStrategy() == OverflowStrategy::NA) {
                    a.mov(asmjit::x86::rax, minimum - 1);
                } else if (expr.GetOverflowStrategy() == OverflowStrategy::WRAPPING) {
                    a.mov(asmjit::x86::rax, minimum);
                } else {
                    EmitArithmeticException(ctx, expr, unwind, "div");
                }
                a.jmp(done);
                a.bind(divide);
                a.cqo();
                a.idiv(asmjit::x86::rcx);
            } else {
                a.bind(divide);
                a.xor_(asmjit::x86::rdx, asmjit::x86::rdx);
                a.div(asmjit::x86::rcx);
            }
            if (kind == K::MOD) {
                a.mov(asmjit::x86::rax, asmjit::x86::rdx);
            }
            a.bind(done);
            break;
        }
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
    if (normal && !ctx.GetFailure()) {
        a.jmp(ctx.GetEdgeLabel(normal));
    }
}
} // namespace Cangjie::JIT::X86_64
