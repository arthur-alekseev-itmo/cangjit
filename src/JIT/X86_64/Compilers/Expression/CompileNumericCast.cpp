// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "CompileNumericCast.h"

#include "CompileBinaryExpression.h"

#include "../../Utils/MemoryUtils.h"
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
    const auto* source = expression.GetSourceValue();
    const auto* result = expression.GetResult();
    const auto* sourceType = source ? source->GetType() : nullptr;
    const auto* targetType = result ? result->GetType() : nullptr;
    if (!sourceType || !targetType) {
        context.Fail(JitFailureKind::UnsupportedNumericCast);
        return;
    }
    const auto sourceKind = sourceType->GetTypeKind();
    const auto targetKind = targetType->GetTypeKind();
    const bool sourceFloat = sourceKind == CHIR::Type::TYPE_FLOAT32 || sourceKind == CHIR::Type::TYPE_FLOAT64;
    const bool targetFloat = targetKind == CHIR::Type::TYPE_FLOAT32 || targetKind == CHIR::Type::TYPE_FLOAT64;
    if (sourceFloat && targetFloat) {
        if (!MemoryUtils::LoadFloatValue(context, source, asmjit::x86::xmm0, asmjit::x86::rax)) {
            return;
        }
        auto& assembler = context.GetAssembler();
        if (sourceKind != targetKind) {
            if (targetKind == CHIR::Type::TYPE_FLOAT64) {
                assembler.cvtss2sd(asmjit::x86::xmm0, asmjit::x86::xmm0);
            } else {
                assembler.cvtsd2ss(asmjit::x86::xmm0, asmjit::x86::xmm0);
            }
        }
        MemoryUtils::StoreFloatValue(context, result, asmjit::x86::xmm0, asmjit::x86::rax);
        if (normal && !context.GetFailure()) {
            assembler.jmp(context.GetLocalFinalizers().GetEdgeLabel(normal));
        }
        return;
    }
    const auto sourceWidth = RegisterUtils::IntegerWidth(*sourceType);
    const auto targetWidth = RegisterUtils::IntegerWidth(*targetType);
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
    if (!MemoryUtils::LoadValue(context, source, asmjit::x86::rax)) {
        return;
    }
    auto& assembler = context.GetAssembler();
    RegisterUtils::NormalizeInteger(assembler, asmjit::x86::rax, sourceWidth, sourceSigned);
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
    RegisterUtils::NormalizeInteger(assembler, asmjit::x86::rax, targetWidth, targetSigned);
    MemoryUtils::StoreValue(context, result, asmjit::x86::rax);
    if (normal && !context.GetFailure()) {
        assembler.jmp(context.GetLocalFinalizers().GetEdgeLabel(normal));
    }
}
} // namespace Cangjie::JIT::X86_64
