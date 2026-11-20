// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "CompileApply.h"

#include "../Context/FunctionContext.h"
#include "../Context/GlobalContext.h"
#include "../Function/CompileFunctionStub.h"
#include "../../Utils/SystemVCallingConvention.h"
#include "../../Utils/MemoryUtils.h"
#include "../../Utils/RegisterUtils.h"
#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Expression/Terminator.h"
#include "cangjie/CHIR/IR/Type/Type.h"
#include "cangjie/CHIR/IR/Value/Value.h"


namespace Cangjie::JIT::X86_64 {
namespace {
namespace x86 = asmjit::x86;

const CHIR::Function* ValidateCall(FunctionContext& context, const CHIR::ApplyBase& expression)
{
    const auto* calleeValue = expression.GetCallee();
    if (!calleeValue || !calleeValue->IsFunc()) {
        context.Fail(JitFailureKind::UnsupportedCall);
        return nullptr;
    }
    const auto* callee = static_cast<const CHIR::Function*>(calleeValue);
    const auto* functionType = callee->GetFuncType();
    const auto* returnType = functionType ? callee->GetReturnType() : nullptr;
    const auto* result = expression.GetResult();
    if (!functionType || functionType->HasVarArg() || callee->IsCFunc() || !returnType ||
        !result || !result->GetType() || result->GetType()->GetTypeKind() != returnType->GetTypeKind()) {
        context.Fail(JitFailureKind::UnsupportedCall);
        return nullptr;
    }
    const bool noResult = returnType->IsUnit() || returnType->IsNothing() || returnType->IsVoid();
    const auto* returnBase = returnType->IsRef()
        ? static_cast<const CHIR::RefType*>(returnType)->GetBaseType() : nullptr;
    const bool objectReturn = returnBase &&
        (returnBase->IsClass() || returnBase->IsRawArray() || returnBase->IsBox());
    if (!noResult && !RegisterUtils::IntegerWidth(*returnType) && !returnType->IsBoolean() &&
        !objectReturn && !returnType->IsCPointer() && !returnType->IsCString()) {
        context.Fail(JitFailureKind::UnsupportedReturnType);
        return nullptr;
    }
    if (expression.GetSuccessorIndex(0) != callee->GetParams().size() + 1) {
        context.Fail(JitFailureKind::UnsupportedCall);
        return nullptr;
    }
    return callee;
}

bool ValidateArguments(FunctionContext& context, const CHIR::ApplyBase& expression, const CHIR::Function& callee)
{
    const auto& parameters = callee.GetParams();
    for (size_t i = 0; i < parameters.size(); ++i) {
        const auto* parameter = parameters[i];
        const auto* argument = expression.GetOperand(i + 1);
        if ((!RegisterUtils::IntegerWidth(*parameter->GetType()) && !parameter->GetType()->IsBoolean() &&
            !parameter->GetType()->IsRef()) ||
            !argument || !argument->GetType() ||
            argument->GetType()->GetTypeKind() != parameter->GetType()->GetTypeKind()) {
            context.Fail(JitFailureKind::UnsupportedCall);
            return false;
        }
    }
    return true;
}

CodeAddress ResolveAddress(FunctionContext& context, const CHIR::Function& callee)
{
    auto& global = context.GetGlobalContext();
    if (callee.IsImportedFunc()) {
        if (!global.GetRuntimeBindings().EnsureStandardPackage(callee.GetPackageName()).empty()) {
            context.Fail(JitFailureKind::UnresolvedSymbol);
            return 0;
        }
        auto symbol = global.GetRuntimeBindings().Resolve(callee.GetIdentifierWithoutPrefix());
        const auto* address = std::get_if<void*>(&symbol);
        if (!address || !*address) {
            context.Fail(JitFailureKind::UnresolvedSymbol);
            return 0;
        }
        return reinterpret_cast<CodeAddress>(*address);
    }
    auto& record = global.GetFunctionRegistry().GetOrCreate(callee);
    if (record.GetState() == FunctionState::FAILED) {
        context.Fail(JitFailureKind::UnsupportedCall);
        return 0;
    }
    const CodeAddress target = record.GetState() == FunctionState::READY
        ? record.GetCompiledTarget() : CompileFunctionStub::Compile(global, &callee);
    if (!target) {
        context.Fail(JitFailureKind::UnsupportedCall);
    }
    return target;
}


bool PlaceArguments(FunctionContext& context, const CHIR::ApplyBase& expression, const CHIR::Function& callee,
    const ParameterPlacement& placement)
{
    MemoryUtils::ReserveStack(context, placement.stackSize);
    const auto& parameters = callee.GetParams();
    for (size_t i = 0; i < parameters.size(); ++i) {
        const auto* stack = placement.locations.find(parameters[i])->second.AsIncomingStack();
        if (!stack) {
            continue;
        }
        if (!MemoryUtils::StoreOutgoingArgument(
            context, expression.GetOperand(i + 1), stack->index, placement.stackSize)) {
            return false;
        }
    }
    for (size_t i = 0; i < parameters.size(); ++i) {
        const auto* location = placement.locations.find(parameters[i])->second.AsRegister();
        if (!location) {
            continue;
        }
        const auto reg = RegisterUtils::Get64BitRegister(location->type);
        if (!reg) {
            context.Fail(JitFailureKind::UnsupportedCallingConvention);
            return false;
        }
        if (!MemoryUtils::LoadValue(context, expression.GetOperand(i + 1), *reg)) {
            return false;
        }
    }
    const auto& typeArgs = expression.GetInstantiatedTypeArgs();
    if (typeArgs.size() != placement.genericTypeInfoLocations.size()) {
        context.Fail(JitFailureKind::UnsupportedCallingConvention);
        return false;
    }
    for (size_t i = 0; i < typeArgs.size(); ++i) {
        if (!MemoryUtils::PlaceTypeInfo(context, typeArgs[i], placement.genericTypeInfoLocations[i], placement.stackSize)) {
            return false;
        }
    }
    if (placement.outerTypeInfoLocation) {
        const auto* outer = expression.GetInstParentCustomTyOfCallee(context.GetGlobalContext().GetCHIRBuilder());
        if (!MemoryUtils::PlaceTypeInfo(context, outer, *placement.outerTypeInfoLocation, placement.stackSize)) {
            return false;
        }
    }
    if (placement.thisTypeInfoLocation &&
        !MemoryUtils::PlaceTypeInfo(context, expression.GetThisType(), *placement.thisTypeInfoLocation, placement.stackSize)) {
        return false;
    }
    const auto* returnType = callee.GetReturnType();
    if ((returnType->IsUnit() || returnType->IsNothing()) &&
        !MemoryUtils::ZeroStackValueAndLoadAddress(context, expression.GetResult(), x86::rdi)) {
        return false;
    }
    return true;
}

void EmitCall(FunctionContext& context, const CHIR::ApplyBase& expression, const CHIR::Function& callee,
    CodeAddress target, uint32_t stackSize, const CHIR::Block* normal, const CHIR::Block* unwind)
{
    auto& assembler = context.GetAssembler();
    const auto instructionOffset = assembler.offset();
    assembler.long_().mov(x86::r11, asmjit::imm(target));
    const auto callStart = assembler.offset();
    assembler.call(x86::r11);
    const auto callEnd = assembler.offset();
    if (!context.RecordSafepoint(expression)) {
        return;
    }
    if (!callee.IsImportedFunc()) {
        auto& record = context.GetGlobalContext().GetFunctionRegistry().GetOrCreate(callee);
        if (target == record.GetEntryStub()) {
            if (assembler.offset() - instructionOffset != 13) {
                context.Fail(JitFailureKind::InvalidLazyCallSite);
                return;
            }
            context.GetLazyCallSites().push_back({instructionOffset + 2, assembler.offset(), &record});
        }
    }
    MemoryUtils::ReleaseStack(context, stackSize);
    const auto* returnType = callee.GetReturnType();
    if (!returnType->IsUnit() && !returnType->IsNothing() && !returnType->IsVoid()) {
        const auto width = returnType->IsBoolean() ? 8U : RegisterUtils::IntegerWidth(*returnType);
        if (width) {
            RegisterUtils::NormalizeInteger(assembler, x86::rax, width, returnType->IsSignedInteger());
        }
        MemoryUtils::StoreValue(context, expression.GetResult(), x86::rax);
    }
    if (normal && unwind && !context.GetFailure()) {
        assembler.jmp(context.GetBlockLabel(normal));
        context.EmitExceptionLanding(*unwind, callStart, callEnd);
    }
}

void CompileCall(FunctionContext& context, const CHIR::ApplyBase& expression,
    const CHIR::Block* normal, const CHIR::Block* unwind)
{
    const auto* callee = ValidateCall(context, expression);
    if (!callee) {
        return;
    }
    const auto placements = SystemVCallingConvention::GetNativeParameterPlacement(*callee);
    if (!placements || !placements->basePointerLocations.empty() ||
        (placements->indirectReturn && !placements->abiReturnType->IsUnit() &&
            !placements->abiReturnType->IsNothing())) {
        context.Fail(JitFailureKind::UnsupportedCallingConvention);
        return;
    }
    if (!ValidateArguments(context, expression, *callee)) {
        return;
    }
    const CodeAddress target = ResolveAddress(context, *callee);
    if (!target || !PlaceArguments(context, expression, *callee, *placements)) {
        return;
    }
    EmitCall(context, expression, *callee, target, placements->stackSize, normal, unwind);
}
} // namespace

void CompileApply::Compile(FunctionContext& context, const CHIR::Apply& expression)
{
    CompileCall(context, expression, nullptr, nullptr);
}

void CompileApply::Compile(FunctionContext& context, const CHIR::TryApply& expression)
{
    if (!expression.GetSuccessBlock() || !expression.GetErrorBlock()) {
        context.Fail(JitFailureKind::MissingBranchDestination);
        return;
    }
    CompileCall(context, expression, expression.GetSuccessBlock(), expression.GetErrorBlock());
}
} // namespace Cangjie::JIT::X86_64
