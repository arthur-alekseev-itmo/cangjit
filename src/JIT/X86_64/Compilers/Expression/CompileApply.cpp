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
#include "../../Utils/TypeUtils.h"
#include "../../Utils/RegisterUtils.h"
#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Expression/Terminator.h"
#include "cangjie/CHIR/IR/Type/Type.h"
#include "cangjie/CHIR/IR/Value/Value.h"
#include "cangjie/Utils/CheckUtils.h"

#include <algorithm>
#include <limits>
#include <vector>

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
    if (!noResult && !TypeUtils::IntegerWidth(*returnType) && !returnType->IsBoolean() &&
        !objectReturn && !returnType->IsCPointer() && !returnType->IsCString() && !TypeUtils::IsFloating(*returnType)) {
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
        if ((!TypeUtils::IntegerWidth(*parameter->GetType()) && !parameter->GetType()->IsBoolean() &&
            !parameter->GetType()->IsRef() && !TypeUtils::IsFloating(*parameter->GetType())) ||
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
        if (!global.GetRuntimeBindings().EnsurePackage(callee.GetPackageName())) {
            context.Fail(JitFailureKind::UnresolvedSymbol);
            return 0;
        }
        auto symbol = global.GetRuntimeBindings().Resolve(callee.GetIdentifierWithoutPrefix());
        if (!symbol) {
            context.Fail(JitFailureKind::UnresolvedSymbol);
            return 0;
        }
        return reinterpret_cast<CodeAddress>(symbol.Value());
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
    const ParameterPlacement& placement, uint32_t& scratchSize)
{
    const auto& typeArgs = expression.GetInstantiatedTypeArgs();
    if (typeArgs.size() != placement.genericTypeInfoLocations.size()) {
        context.Fail(JitFailureKind::UnsupportedCallingConvention);
        return false;
    }
    const auto* outer = placement.outerTypeInfoLocation
        ? expression.GetInstParentCustomTyOfCallee(context.GetGlobalContext().GetCHIRBuilder()) : nullptr;
    const auto* thisType = placement.thisTypeInfoLocation ? expression.GetThisType() : nullptr;
    const auto dynamicType = [](const CHIR::Type* type) {
        return type && !type->IsGeneric() && !type->IsThis() && type->IsGenericRelated();
    };
    const bool needsScratch = std::any_of(typeArgs.begin(), typeArgs.end(), dynamicType) ||
        dynamicType(outer) || dynamicType(thisType);
    if (needsScratch) {
        std::vector<const CHIR::Type*> types(typeArgs.begin(), typeArgs.end());
        if (placement.outerTypeInfoLocation) {
            types.push_back(outer);
        }
        if (placement.thisTypeInfoLocation) {
            types.push_back(thisType);
        }
        const auto size = MemoryUtils::StageTypeInfos(context, types);
        if (!size || placement.stackSize > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
            *size > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) - placement.stackSize) {
            context.Fail(JitFailureKind::UnsupportedCallingConvention);
            return false;
        }
        scratchSize = *size;
    }
    MemoryUtils::ReserveStack(context, placement.stackSize);
    const auto& parameters = callee.GetParams();
    for (size_t i = 0; i < parameters.size(); ++i) {
        const auto location = placement.locations.find(parameters[i]);
        CJC_ASSERT(location != placement.locations.end());
        if (location == placement.locations.end()) {
            context.Fail(JitFailureKind::UnsupportedCallingConvention);
            return false;
        }
        const auto* stack = location->second.AsIncomingStack();
        if (!stack) {
            continue;
        }
        if (!MemoryUtils::StoreOutgoingArgument(
            context, expression.GetOperand(i + 1), stack->index, placement.stackSize)) {
            return false;
        }
    }
    for (size_t i = 0; i < parameters.size(); ++i) {
        const auto it = placement.locations.find(parameters[i]);
        CJC_ASSERT(it != placement.locations.end());
        if (it == placement.locations.end()) {
            context.Fail(JitFailureKind::UnsupportedCallingConvention);
            return false;
        }
        const auto& location = it->second;
        if (const auto* reg = location.AsGpRegister()) {
            const auto target = RegisterUtils::Get64BitRegister(reg->type);
            if (!target || !MemoryUtils::LoadValue(context, expression.GetOperand(i + 1), *target)) {
                context.Fail(JitFailureKind::UnsupportedCallingConvention);
                return false;
            }
        } else if (const auto* simd = location.AsSimdRegister()) {
            const auto target = RegisterUtils::GetArgumentSimdRegister(simd->index);
            if (!target || !MemoryUtils::LoadFloatValue(context, expression.GetOperand(i + 1), *target, x86::rax)) {
                context.Fail(JitFailureKind::UnsupportedCallingConvention);
                return false;
            }
        }
    }
    size_t typeIndex = 0;
    const auto placeType = [&](const CHIR::Type* type, const Location& location) {
        if (needsScratch) {
            const auto offset = static_cast<int32_t>(placement.stackSize + typeIndex++ * sizeof(uint64_t));
            return MemoryUtils::PlaceStagedTypeInfo(context, location, placement.stackSize, offset);
        }
        return MemoryUtils::PlaceTypeInfo(context, type, location, placement.stackSize);
    };
    for (size_t i = 0; i < typeArgs.size(); ++i) {
        if (!placeType(typeArgs[i], placement.genericTypeInfoLocations[i])) {
            return false;
        }
    }
    if (placement.outerTypeInfoLocation && !placeType(outer, *placement.outerTypeInfoLocation)) {
        return false;
    }
    if (placement.thisTypeInfoLocation && !placeType(thisType, *placement.thisTypeInfoLocation)) {
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
    if (!context.GetSafepointRecorder().Record(expression)) {
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
        if (TypeUtils::IsFloating(*returnType)) {
            if (!MemoryUtils::StoreFloatValue(context, expression.GetResult(), x86::xmm0, x86::rax)) {
                return;
            }
        } else {
            const auto width = TypeUtils::TypeWidth(*returnType);
            if (width) {
                TypeUtils::NormalizeInteger(assembler, x86::rax, width, returnType->IsSignedInteger());
            }
            MemoryUtils::StoreValue(context, expression.GetResult(), x86::rax);
        }
    }
    if (normal && unwind && !context.GetFailure()) {
        assembler.jmp(context.GetBlockLabel(normal));
        context.GetExceptionLandingEmitter().Emit(*unwind, callStart, callEnd);
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
    uint32_t scratchSize = 0;
    if (!target || !PlaceArguments(context, expression, *callee, *placements, scratchSize)) {
        return;
    }
    EmitCall(context, expression, *callee, target, placements->stackSize + scratchSize, normal, unwind);
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
