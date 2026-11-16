// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "CompileFunction.h"

#include <iostream>
#include <utility>

#include "../RegisterAllocation/LivenessInterferenceGraphBuilder.h"
#include "../RegisterAllocation/TrivialStackRegalloc.h"
#include "../RegisterAllocation/ValueNumeration.h"
#include "../Utils/MemoryUtils.h"
#include "../Utils/StackFrameUtils.h"
#include "CompileBlock.h"
#include "Context/FunctionContext.h"
#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Value/Value.h"

namespace Cangjie::JIT::X86_64 {

CompileFunction::CompileFunction(GlobalContext& globalContext, const CHIR::Function& function) noexcept
    : globalContext(globalContext), function(function)
{
}

void CompileFunction::Compile(std::ostream& output) const
{
    auto valueNumeration = ValueNumerator(function).Run();
    auto interferenceGraph = LivenessInterferenceGraphBuilder().Build(function, valueNumeration);
    auto registerAllocation = TrivialStackRegalloc().Allocate(valueNumeration, interferenceGraph);
    FunctionContext context(globalContext, &function, std::move(valueNumeration), std::move(interferenceGraph),
        std::move(registerAllocation));
    if (context.GetError() != asmjit::kErrorOk) {
        std::cerr << "JIT initialization failed: " << asmjit::DebugUtils::errorAsString(context.GetError()) << '\n';
        output.setstate(std::ios::badbit);
        return;
    }
    context.Dump(output);
    Compile(context);
    if (const auto& failure = context.GetFailure()) {
        std::cerr << "JIT compilation failed: " << GetJitFailureMessage(failure->kind);
        if (failure->expressionKind) {
            std::cerr << ": " << CHIR::ExprKindMgr::Instance()->GetKindName(
                static_cast<size_t>(*failure->expressionKind));
        }
        std::cerr << '\n';
        output.setstate(std::ios::badbit);
    } else if (context.GetError() != asmjit::kErrorOk) {
        std::cerr << "JIT emission failed: " << asmjit::DebugUtils::errorAsString(context.GetError()) << '\n';
        output.setstate(std::ios::badbit);
    }
}

void CompileFunction::MakePrologue(FunctionContext& context) const
{
    const auto size = StackFrameUtils::GetFrameSize(context.GetRegisterAllocation().GetStackSlotCount());
    if (!size) {
        context.Fail(JitFailureKind::StackFrameTooLarge);
        return;
    }
    auto& assembler = context.GetAssembler();
    assembler.push(asmjit::x86::rbp);
    assembler.mov(asmjit::x86::rbp, asmjit::x86::rsp);
    if (*size != 0) {
        assembler.sub(asmjit::x86::rsp, *size);
    }
}

void CompileFunction::MakeEpilogue(FunctionContext& context) const
{
    auto& assembler = context.GetAssembler();
    assembler.bind(context.GetEpilogueLabel());
    const auto* type = function.GetReturnType();
    if (!type->IsUnit() && !type->IsVoid() && !type->IsNothing()) {
        if (type->GetTypeKind() != CHIR::Type::TYPE_INT64 && type->GetTypeKind() != CHIR::Type::TYPE_UINT64) {
            context.Fail(JitFailureKind::UnsupportedReturnType);
            return;
        }
        if (!MemoryUtils::LoadValue(context, function.GetReturnValue(), asmjit::x86::rax)) {
            return;
        }
        assembler.mov(asmjit::x86::rax, asmjit::x86::qword_ptr(asmjit::x86::rax));
    }
    assembler.mov(asmjit::x86::rsp, asmjit::x86::rbp);
    assembler.pop(asmjit::x86::rbp);
    assembler.ret();
}

void CompileFunction::Compile(FunctionContext& context) const
{
    if (context.GetFunction() != &function || !function.GetBody() || !function.GetBody()->GetEntryBlock()) {
        context.Fail(JitFailureKind::InvalidFunctionContext);
        return;
    }
    if (context.GetValueNumeration().GetParameterCount() != 0) {
        context.Fail(JitFailureKind::UnsupportedParameters);
        return;
    }
    MakePrologue(context);
    if (context.GetFailure()) {
        return;
    }
    auto& assembler = context.GetAssembler();
    assembler.jmp(context.GetBlockLabel(function.GetBody()->GetEntryBlock()));
    for (const auto* block : function.GetBody()->GetBlocks()) {
        CompileBlock::Compile(context, *block);
        if (context.GetFailure() || context.GetError() != asmjit::kErrorOk) {
            return;
        }
    }
    MakeEpilogue(context);
}

} // namespace Cangjie::JIT::X86_64
