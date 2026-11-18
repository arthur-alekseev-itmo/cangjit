// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "CompileConstant.h"

#include "../../Utils/MemoryUtils.h"
#include "../Context/FunctionContext.h"
#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Value/LiteralValue.h"

namespace Cangjie::JIT::X86_64 {
void CompileConstant::Compile(FunctionContext& context, const CHIR::Constant& expression)
{
    if (MemoryUtils::LoadValue(context, expression.GetValue(), asmjit::x86::rax)) {
        MemoryUtils::StoreValue(context, expression.GetResult(), asmjit::x86::rax);
    }
}
} // namespace Cangjie::JIT::X86_64
