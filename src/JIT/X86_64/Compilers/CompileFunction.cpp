// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "CompileFunction.h"

#include <utility>

#include "../RegisterAllocation/LivenessInterferenceGraphBuilder.h"
#include "../RegisterAllocation/TrivialStackRegalloc.h"
#include "../RegisterAllocation/ValueNumeration.h"
#include "Context/FunctionContext.h"

namespace Cangjie::JIT::X86_64 {

CompileFunction::CompileFunction(const CHIR::Function& function) noexcept : function(function)
{
}

void CompileFunction::Compile(std::ostream& output) const
{
    auto valueNumeration = ValueNumerator(function).Run();
    auto interferenceGraph = LivenessInterferenceGraphBuilder().Build(function, valueNumeration);
    auto registerAllocation = TrivialStackRegalloc().Allocate(valueNumeration, interferenceGraph);
    FunctionContext context(&function, std::move(valueNumeration), std::move(interferenceGraph),
        std::move(registerAllocation));
    context.Dump(output);
}

} // namespace Cangjie::JIT::X86_64
