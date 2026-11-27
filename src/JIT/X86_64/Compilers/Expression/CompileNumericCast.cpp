// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "CompileNumericCast.h"

#include "CompileBinaryExpression.h"

#include "../../Utils/MemoryUtils.h"
#include "../../Utils/TypeUtils.h"
#include "../../Utils/RegisterUtils.h"
#include "../Context/FunctionContext.h"
#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Type/Type.h"

#include <cstdint>
#include <string>

namespace Cangjie::JIT::X86_64 {
void CompileNumericCast::Compile(FunctionContext& context, const CHIR::NumericCastBase& expression,
    const CHIR::Block* normal, const CHIR::Block* unwind)
{
    auto source = expression.GetSourceValue();
    auto result = expression.GetResult();
    auto sourceType = source ? source->GetType() : nullptr;
    auto targetType = result ? result->GetType() : nullptr;
    if (!sourceType || !targetType) {
        context.Fail(JitFailureKind::UnsupportedNumericCast);
        return;
    }
    const auto sourceKind = sourceType->GetTypeKind();
    const auto targetKind = targetType->GetTypeKind();
    const bool sourceFloat = TypeUtils::IsFloating(*sourceType);
    const bool targetFloat = TypeUtils::IsFloating(*targetType);
    if (sourceFloat && targetFloat) {
        CompileFloatCast(context, source, result, sourceKind != targetKind,
            targetKind == CHIR::Type::TYPE_FLOAT64, normal);
        return;
    }
    const auto sourceWidth = TypeUtils::IntegerWidth(*sourceType);
    const auto targetWidth = TypeUtils::IntegerWidth(*targetType);
    if (!sourceWidth || !targetWidth) {
        context.Fail(JitFailureKind::UnsupportedNumericCast);
        return;
    }
    const bool sourceSigned = sourceType->IsSignedInteger();
    const bool targetSigned = targetType->IsSignedInteger();
    const bool alwaysFits = sourceSigned == targetSigned ? targetWidth >= sourceWidth :
        !sourceSigned && targetSigned && targetWidth > sourceWidth;
    const auto overflow = expression.GetOverflowStrategy();
    if (overflow != OverflowStrategy::NA && overflow != OverflowStrategy::WRAPPING &&
        overflow != OverflowStrategy::SATURATING && overflow != OverflowStrategy::THROWING) {
        context.Fail(JitFailureKind::UnsupportedNumericCastOverflow);
        return;
    }
    auto sourceLocation = context.GetValueLocation(source);
    if (!MemoryUtils::Move(context, RegisterUtils::ToLocation(asmjit::x86::rax),
        *sourceLocation, *sourceType, asmjit::x86::r11)) {
        return;
    }
    auto& assembler = context.GetAssembler();
    TypeUtils::NormalizeInteger(assembler, asmjit::x86::rax, sourceWidth, sourceSigned);
    if (!alwaysFits && (overflow == OverflowStrategy::SATURATING || overflow == OverflowStrategy::THROWING)) {
        const uint64_t targetSignBit = uint64_t{1} << (targetWidth - 1);
        const uint64_t lower = targetSigned ? ~(targetSignBit - 1) : 0;
        const uint64_t upper = targetSigned ? targetSignBit - 1 :
            (targetWidth == 64 ? UINT64_MAX : (uint64_t{1} << targetWidth) - 1);
        auto clampLower = assembler.newLabel();
        auto clampUpper = assembler.newLabel();
        auto done = assembler.newLabel();
        if (sourceSigned && (!targetSigned || sourceWidth > targetWidth)) {
            assembler.mov(asmjit::x86::rcx, lower);
            assembler.cmp(asmjit::x86::rax, asmjit::x86::rcx);
            assembler.jl(clampLower);
        }
        if ((targetSigned && (!sourceSigned || sourceWidth > targetWidth)) ||
            (!targetSigned && targetWidth < sourceWidth)) {
            assembler.mov(asmjit::x86::rcx, upper);
            assembler.cmp(asmjit::x86::rax, asmjit::x86::rcx);
            if (sourceSigned) {
                assembler.jg(clampUpper);
            } else {
                assembler.ja(clampUpper);
            }
        }
        assembler.jmp(done);
        assembler.bind(clampLower);
        if (overflow == OverflowStrategy::THROWING) {
            const auto name = targetType->ToString();
            EmitArithmeticException(context, expression, unwind,
                ("The result would be less than " + name + ".Min.").c_str());
            assembler.bind(clampUpper);
            EmitArithmeticException(context, expression, unwind,
                ("The result would be greater than " + name + ".Max.").c_str());
        } else {
            assembler.mov(asmjit::x86::rax, lower);
            assembler.jmp(done);
            assembler.bind(clampUpper);
            assembler.mov(asmjit::x86::rax, upper);
        }
        assembler.bind(done);
    }
    TypeUtils::NormalizeInteger(assembler, asmjit::x86::rax, targetWidth, targetSigned);
    auto resultLocation = context.GetValueLocation(result);
    if (!MemoryUtils::Move(context, *resultLocation, RegisterUtils::ToLocation(asmjit::x86::rax),
        *targetType, asmjit::x86::r11)) {
        return;
    }
    if (normal && !context.GetFailure()) {
        assembler.jmp(context.GetBlockLabel(normal));
    }
}

void CompileNumericCast::CompileFloatCast(FunctionContext& context, const CHIR::Value* source, const CHIR::Value* result,
    bool needsConversion, bool targetIsFloat64, const CHIR::Block* normal)
{
    auto sourceLocation = context.GetValueLocation(source);
    if (!MemoryUtils::Move(context, RegisterUtils::ToLocation(asmjit::x86::xmm0),
        *sourceLocation, *source->GetType(), asmjit::x86::rax)) {
        return;
    }
    auto& assembler = context.GetAssembler();
    if (needsConversion && targetIsFloat64) {
        assembler.cvtss2sd(asmjit::x86::xmm0, asmjit::x86::xmm0);
    } else if (needsConversion) {
        assembler.cvtsd2ss(asmjit::x86::xmm0, asmjit::x86::xmm0);
    }
    auto resultLocation = context.GetValueLocation(result);
    if (!MemoryUtils::Move(context, *resultLocation,
        RegisterUtils::ToLocation(asmjit::x86::xmm0), *result->GetType(), asmjit::x86::rax)) {
        return;
    }
    if (normal && !context.GetFailure()) {
        assembler.jmp(context.GetBlockLabel(normal));
    }
}
} // namespace Cangjie::JIT::X86_64
