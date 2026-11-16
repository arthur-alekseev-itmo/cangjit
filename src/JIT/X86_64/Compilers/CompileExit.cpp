// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "CompileExit.h"

#include "Context/FunctionContext.h"

namespace Cangjie::JIT::X86_64 {
void CompileExit::Compile(FunctionContext& ctx, const CHIR::Exit&)
{
    ctx.GetAssembler().jmp(ctx.GetEpilogueLabel());
}
} // namespace Cangjie::JIT::X86_64
