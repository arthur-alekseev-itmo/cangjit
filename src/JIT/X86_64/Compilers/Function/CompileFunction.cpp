// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "CompileFunction.h"

#include <iostream>
#include <limits>
#include <utility>

#include "../../RegisterAllocation/LivenessInterferenceGraphBuilder.h"
#include "../../RegisterAllocation/TrivialStackRegalloc.h"
#include "../../RegisterAllocation/ValueNumeration.h"
#include "../../Utils/SystemVCallingConvention.h"
#include "../../Utils/MemoryUtils.h"
#include "../../Utils/RegisterUtils.h"
#include "../../Utils/StackFrameUtils.h"
#include "CompileBlock.h"
#include "../Context/GlobalContext.h"
#include "../Context/FunctionContext.h"
#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Type/Type.h"
#include "cangjie/CHIR/IR/Value/Value.h"

namespace Cangjie::JIT::X86_64 {

CompileFunction::CompileFunction(GlobalContext& globalContext, const CHIR::Function& function) noexcept
    : globalContext(globalContext), function(function)
{
}


CompilationResult CompileFunction::CompileToAddress(std::ostream* output) const
{
#if !defined(__x86_64__) || defined(_WIN32)
    (void)output;
    return CompilationError{"only the x86-64 System V ABI is supported"};
#else
    if (!function.GetBody() || !function.GetBody()->GetEntryBlock() || !function.GetReturnType() ||
        !function.GetFuncType()) {
        return CompilationError{"invalid function"};
    }
    auto valueNumeration = ValueNumerator(function).Run();
    auto interferenceGraph = LivenessInterferenceGraphBuilder().Build(function, valueNumeration);
    auto registerAllocation = TrivialStackRegalloc().Allocate(valueNumeration, interferenceGraph);
    FunctionContext context(globalContext, &function, std::move(valueNumeration), std::move(interferenceGraph),
        std::move(registerAllocation));
    if (context.GetError() == asmjit::kErrorOk && !context.GetFailure()) {
        if (output) {
            context.Dump(*output);
            if (!*output) {
                return CompilationError{"failed to write JIT dump"};
            }
        }
        Compile(context);
    }
    if (const auto& failure = context.GetFailure()) {
        std::string message = GetJitFailureMessage(failure->kind);
        if (failure->expressionKind) {
            message += ": ";
            message += CHIR::ExprKindMgr::Instance()->GetKindName(static_cast<size_t>(*failure->expressionKind));
        }
        return CompilationError{std::move(message)};
    }
    if (context.GetError() != asmjit::kErrorOk) {
        return CompilationError{asmjit::DebugUtils::errorAsString(context.GetError())};
    }
    void* address = nullptr;
    const auto error = globalContext.GetRuntime().add(&address, &context.GetCode());
    if (error != asmjit::kErrorOk) {
        return CompilationError{asmjit::DebugUtils::errorAsString(error)};
    }
    if (!globalContext.GetLazyCallSiteRegistry().Register(
            reinterpret_cast<CodeAddress>(address), context.GetCode().codeSize(), context.GetLazyCallSites())) {
        globalContext.GetRuntime().release(address);
        return CompilationError{"failed to register lazy call sites"};
    }
    return reinterpret_cast<CodeAddress>(address);
#endif
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
    const auto placements = SystemVCallingConvention::GetParameterPlacement(function);
    if (!placements) {
        context.Fail(JitFailureKind::UnsupportedCallingConvention);
        return;
    }
    MakePrologue(context);
    if (context.GetFailure()) {
        return;
    }
    for (const auto* parameter : function.GetParams()) {
        if (!parameter || !parameter->GetType()) {
            context.Fail(JitFailureKind::UnsupportedParameters);
            return;
        }
        const auto width = RegisterUtils::IntegerWidth(*parameter->GetType());
        const auto location = placements->locations.find(parameter);
        const auto id = context.GetValueNumeration().GetId(parameter);
        if (!width || location == placements->locations.end() || !id ||
            *id >= context.GetRegisterAllocation().GetLocationCount() ||
            !context.GetRegisterAllocation().GetLocation(*id).IsStack()) {
            context.Fail(JitFailureKind::UnsupportedParameters);
            return;
        }
        auto& a = context.GetAssembler();
        if (const auto* reg = location->second.AsRegister()) {
            const auto source = RegisterUtils::Get64BitRegister(reg->type);
            if (!source) {
                context.Fail(JitFailureKind::UnsupportedParameters);
                return;
            }
            a.mov(asmjit::x86::rax, *source);
        } else if (const auto* stack = location->second.AsIncomingStack()) {
            constexpr size_t BASE_OFFSET = 2 * sizeof(uint64_t);
            if (stack->index > (static_cast<size_t>(std::numeric_limits<int32_t>::max()) - BASE_OFFSET) /
                    sizeof(uint64_t)) {
                context.Fail(JitFailureKind::UnsupportedParameters);
                return;
            }
            const auto offset = static_cast<int32_t>(BASE_OFFSET + stack->index * sizeof(uint64_t));
            a.mov(asmjit::x86::rax, asmjit::x86::qword_ptr(asmjit::x86::rbp, offset));
        } else {
            context.Fail(JitFailureKind::UnsupportedParameters);
            return;
        }
        RegisterUtils::NormalizeInteger(a, asmjit::x86::rax, width, parameter->GetType()->IsSignedInteger());
        if (!MemoryUtils::StoreValue(context, parameter, asmjit::x86::rax)) {
            return;
        }
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
