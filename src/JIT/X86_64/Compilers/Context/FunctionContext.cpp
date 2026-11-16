// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "FunctionContext.h"

#include <cassert>
#include <ostream>
#include <utility>

#include "cangjie/CHIR/IR/Value/Value.h"

namespace Cangjie::JIT::X86_64 {

FunctionContext::FunctionContext(const CHIR::Function* function, ValueNumeration valueNumeration,
    InterferenceGraph interferenceGraph, RegisterAllocation registerAllocation) noexcept
    : function(function),
      valueNumeration(std::move(valueNumeration)),
      interferenceGraph(std::move(interferenceGraph)),
      registerAllocation(std::move(registerAllocation))
{
    assert(function != nullptr);
}

const CHIR::Function* FunctionContext::GetFunction() const noexcept
{
    return function;
}

const ValueNumeration& FunctionContext::GetValueNumeration() const noexcept
{
    return valueNumeration;
}

const InterferenceGraph& FunctionContext::GetInterferenceGraph() const noexcept
{
    return interferenceGraph;
}

const RegisterAllocation& FunctionContext::GetRegisterAllocation() const noexcept
{
    return registerAllocation;
}

void FunctionContext::Dump(std::ostream& output) const
{
    output << "FunctionContext(function=" << function->GetIdentifier() << ")\n";
    output << "Body:\n" << function->GetBody()->ToString(0) << '\n';
    valueNumeration.Dump(output);
    interferenceGraph.Dump(output);
    registerAllocation.Dump(output);
}

} // namespace Cangjie::JIT::X86_64
