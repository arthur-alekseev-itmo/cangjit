// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "CompileNumericCast.h"

#include "../../Utils/MemoryUtils.h"
#include "../../Utils/RegisterUtils.h"
#include "../Context/FunctionContext.h"
#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Type/Type.h"

namespace Cangjie::JIT::X86_64 {
void CompileNumericCast::Compile(FunctionContext& context, const CHIR::NumericCast& expression)
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
    if (!alwaysFits && overflow != OverflowStrategy::NA && overflow != OverflowStrategy::WRAPPING) {
        context.Fail(JitFailureKind::UnsupportedNumericCastOverflow);
        return;
    }
    if (!MemoryUtils::LoadValue(context, source, asmjit::x86::rax)) {
        return;
    }
    auto& assembler = context.GetAssembler();
    RegisterUtils::NormalizeInteger(assembler, asmjit::x86::rax, sourceWidth, sourceSigned);
    RegisterUtils::NormalizeInteger(assembler, asmjit::x86::rax, targetWidth, targetSigned);
    MemoryUtils::StoreValue(context, result, asmjit::x86::rax);
}
} // namespace Cangjie::JIT::X86_64
