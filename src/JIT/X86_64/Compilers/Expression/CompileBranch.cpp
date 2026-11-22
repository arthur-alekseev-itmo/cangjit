// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "CompileBranch.h"

#include "../../Utils/MemoryUtils.h"
#include "../Context/FunctionContext.h"
#include "cangjie/CHIR/IR/Expression/Terminator.h"
#include "cangjie/CHIR/IR/Type/Type.h"
#include "cangjie/CHIR/IR/Value/Value.h"

namespace Cangjie::JIT::X86_64 {
void CompileBranch::Compile(FunctionContext& ctx, const CHIR::Branch& expr)
{
    auto* condition = expr.GetCondition();
    auto* trueBlock = expr.GetTrueBlock();
    auto* falseBlock = expr.GetFalseBlock();
    if (!condition || !condition->GetType() || !condition->GetType()->IsBoolean() || !trueBlock || !falseBlock) {
        ctx.Fail(JitFailureKind::InvalidBranch);
        return;
    }
    if (!MemoryUtils::LoadValue(ctx, condition, asmjit::x86::rax)) {
        return;
    }
    auto& assembler = ctx.GetAssembler();
    assembler.test(asmjit::x86::rax, asmjit::x86::rax);
    assembler.jnz(ctx.GetLocalFinalizers().GetEdgeLabel(trueBlock));
    assembler.jmp(ctx.GetLocalFinalizers().GetEdgeLabel(falseBlock));
}
} // namespace Cangjie::JIT::X86_64
