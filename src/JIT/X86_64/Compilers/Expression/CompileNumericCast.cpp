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
            const char* typeName = nullptr;
            switch (targetType->GetTypeKind()) {
                case CHIR::Type::TYPE_INT8: typeName = "Int8"; break;
                case CHIR::Type::TYPE_INT16: typeName = "Int16"; break;
                case CHIR::Type::TYPE_INT32: typeName = "Int32"; break;
                case CHIR::Type::TYPE_INT64: typeName = "Int64"; break;
                case CHIR::Type::TYPE_INT_NATIVE: typeName = "IntNative"; break;
                case CHIR::Type::TYPE_UINT8: typeName = "UInt8"; break;
                case CHIR::Type::TYPE_UINT16: typeName = "UInt16"; break;
                case CHIR::Type::TYPE_UINT32: typeName = "UInt32"; break;
                case CHIR::Type::TYPE_UINT64: typeName = "UInt64"; break;
                case CHIR::Type::TYPE_UINT_NATIVE: typeName = "UIntNative"; break;
                default: context.Fail(JitFailureKind::UnsupportedNumericCast); return;
            }
            const std::string prefix = std::string("CJ_JIT_Throw_Cast") + typeName;
            EmitArithmeticException(context, expression, unwind, (prefix + "Min").c_str());
            assembler.bind(clampUpper);
            EmitArithmeticException(context, expression, unwind, (prefix + "Max").c_str());
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
        assembler.jmp(context.GetBlockLabel(normal));
    }
}
} // namespace Cangjie::JIT::X86_64
