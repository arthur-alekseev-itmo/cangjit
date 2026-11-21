// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "CompileGoTo.h"

#include "../Context/FunctionContext.h"
#include "cangjie/CHIR/IR/Expression/Terminator.h"

namespace Cangjie::JIT::X86_64 {
void CompileGoTo::Compile(FunctionContext& ctx, const CHIR::GoTo& expr)
{
    auto* destination = expr.GetDestination();
    if (!destination) {
        ctx.Fail(JitFailureKind::MissingGoToDestination);
        return;
    }
    ctx.GetAssembler().jmp(ctx.GetEdgeLabel(destination));
}
} // namespace Cangjie::JIT::X86_64
