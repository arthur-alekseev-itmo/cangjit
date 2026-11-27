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
#include "../../Utils/StackFrameUtils.h"
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
    auto calleeValue = expression.GetCallee();
    if (!calleeValue || !calleeValue->IsFunc()) {
        context.Fail(JitFailureKind::UnsupportedCall);
        return nullptr;
    }
    auto callee = static_cast<const CHIR::Function*>(calleeValue);
    auto functionType = callee->GetFuncType();
    auto returnType = functionType ? callee->GetReturnType() : nullptr;
    auto result = expression.GetResult();
    if (!functionType || functionType->HasVarArg() || callee->IsCFunc() || !returnType ||
        !result || !result->GetType() || result->GetType()->GetTypeKind() != returnType->GetTypeKind()) {
        context.Fail(JitFailureKind::UnsupportedCall);
        return nullptr;
    }
    const bool noResult = returnType->IsUnit() || returnType->IsNothing() || returnType->IsVoid();
    auto returnBase = returnType->IsRef()
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
        auto parameter = parameters[i];
        auto argument = expression.GetOperand(i + 1);
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
Location OutgoingLocation(const Location& location)
{
    if (auto stack = location.AsIncomingStack()) {
        return Location::Stack(stack->index, Location::StackBase::Rsp);
    }
    return location;
}

bool PlaceTypeInfo(FunctionContext& context, const CHIR::Type* type, const Location& location)
{
    if (!type) {
        context.Fail(JitFailureKind::UnsupportedCallingConvention);
        return false;
    }
    if (auto reg = location.AsGpRegister()) {
        const auto target = RegisterUtils::Get64BitRegister(reg->type);
        if (!target) {
            context.Fail(JitFailureKind::UnsupportedCallingConvention);
            return false;
        }
        return context.GetTypeInfoEmitter().LoadTypeInfo(*type, *target);
    }
    auto stack = location.AsIncomingStack();
    if (!stack) {
        context.Fail(JitFailureKind::UnsupportedCallingConvention);
        return false;
    }
    if (!context.GetTypeInfoEmitter().LoadTypeInfo(*type, x86::rax)) {
        return false;
    }
    context.GetAssembler().mov(x86::qword_ptr(x86::rsp, static_cast<int32_t>(stack->index * sizeof(uint64_t))),
        x86::rax);
    return context.GetError() == asmjit::kErrorOk;
}

bool PlaceStagedTypeInfo(FunctionContext& context, const Location& location, int32_t offset)
{
    auto& assembler = context.GetAssembler();
    if (auto reg = location.AsGpRegister()) {
        const auto target = RegisterUtils::Get64BitRegister(reg->type);
        if (!target) {
            context.Fail(JitFailureKind::UnsupportedCallingConvention);
            return false;
        }
        assembler.mov(*target, x86::qword_ptr(x86::rsp, offset));
        return context.GetError() == asmjit::kErrorOk;
    }
    auto stack = location.AsIncomingStack();
    if (!stack) {
        context.Fail(JitFailureKind::UnsupportedCallingConvention);
        return false;
    }
    assembler.mov(x86::rax, x86::qword_ptr(x86::rsp, offset));
    assembler.mov(x86::qword_ptr(x86::rsp, static_cast<int32_t>(stack->index * sizeof(uint64_t))), x86::rax);
    return context.GetError() == asmjit::kErrorOk;
}

bool PrepareIndirectReturn(FunctionContext& context, const CHIR::Value* result, const x86::Gp& target)
{
    auto location = context.GetValueLocation(result);
    auto stack = location->AsStack();
    if (!stack || stack->base != Location::StackBase::Frame) {
        context.Fail(JitFailureKind::UnallocatedResult);
        return false;
    }
    const auto offset = StackFrameUtils::GetSlotOffset(
        stack->index, context.GetRegisterAllocation().GetStackSlotCount());
    if (!offset || !result->GetType() ||
        !MemoryUtils::StoreConstant(context, 0, *location, *result->GetType(), x86::rax)) {
        return false;
    }
    context.GetAssembler().lea(target, x86::qword_ptr(x86::rbp, *offset));
    return context.GetError() == asmjit::kErrorOk;
}

bool PlaceArguments(FunctionContext& context, const CHIR::ApplyBase& expression, const CHIR::Function& callee,
    const ParameterPlacement& placement, size_t& scratchSize)
{
    const auto& typeArgs = expression.GetInstantiatedTypeArgs();
    CJC_ASSERT(typeArgs.size() == placement.genericTypeInfoLocations.size());
    auto outer = placement.outerTypeInfoLocation
        ? expression.GetInstParentCustomTyOfCallee(context.GetGlobalContext().GetCHIRBuilder()) : nullptr;
    auto thisType = placement.thisTypeInfoLocation ? expression.GetThisType() : nullptr;
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
        if (types.size() > (static_cast<size_t>(std::numeric_limits<int32_t>::max()) - 15) / sizeof(uint64_t)) {
            context.Fail(JitFailureKind::UnsupportedCallingConvention);
            return false;
        }
        scratchSize = static_cast<uint32_t>((types.size() * sizeof(uint64_t) + 15) & ~size_t{15});
        context.GetAssembler().sub(x86::rsp, scratchSize);
        if (!context.GetTypeInfoEmitter().StoreTypeInfos(types, 0)) {
            return false;
        }
    }
    if (placement.stackSize > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
        scratchSize > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) - placement.stackSize) {
        context.Fail(JitFailureKind::UnsupportedCallingConvention);
        return false;
    }
    context.GetAssembler().sub(x86::rsp, placement.stackSize);
    const auto& parameters = callee.GetParams();
    for (size_t i = 0; i < parameters.size(); ++i) {
        const auto location = placement.locations.find(parameters[i]);
        CJC_ASSERT(location != placement.locations.end());
        if (location == placement.locations.end()) {
            context.Fail(JitFailureKind::UnsupportedCallingConvention);
            return false;
        }
        auto stack = location->second.AsIncomingStack();
        if (!stack) {
            continue;
        }
        auto value = expression.GetOperand(i + 1);
        auto source = context.GetValueLocation(value);
        if (!value->GetType() ||
            !MemoryUtils::Move(context, Location::Stack(stack->index, Location::StackBase::Rsp), *source,
                *value->GetType(), x86::rax)) {
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
        if (location.AsIncomingStack()) {
            continue;
        }
        auto value = expression.GetOperand(i + 1);
        auto source = context.GetValueLocation(value);
        if (!value->GetType() ||
            !MemoryUtils::Move(context, OutgoingLocation(location), *source, *value->GetType(), x86::r11)) {
            return false;
        }
    }
    size_t typeIndex = 0;
    const auto placeType = [&](const CHIR::Type* type, const Location& location) {
        if (!needsScratch) {
            return PlaceTypeInfo(context, type, location);
        }
        if (!type) {
            context.Fail(JitFailureKind::UnsupportedCallingConvention);
            return false;
        }
        const auto offset = static_cast<int32_t>(placement.stackSize + typeIndex++ * sizeof(uint64_t));
        return PlaceStagedTypeInfo(context, location, offset);
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
    auto returnType = callee.GetReturnType();
    if ((returnType->IsUnit() || returnType->IsNothing()) &&
        !PrepareIndirectReturn(context, expression.GetResult(), x86::rdi)) {
        return false;
    }
    return true;
}


void EmitCall(FunctionContext& context, const CHIR::ApplyBase& expression, const CHIR::Function& callee,
    CodeAddress target, size_t stackSize, const CHIR::Block* normal, const CHIR::Block* unwind)
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
    assembler.add(x86::rsp, stackSize);
    auto returnType = callee.GetReturnType();
    if (!returnType->IsUnit() && !returnType->IsNothing() && !returnType->IsVoid()) {
        auto resultLocation = context.GetValueLocation(expression.GetResult());
        if (TypeUtils::IsFloating(*returnType)) {
            if (!MemoryUtils::Move(context, *resultLocation, RegisterUtils::ToLocation(x86::xmm0),
                *returnType, x86::rax)) {
                return;
            }
        } else if (!MemoryUtils::Move(context, *resultLocation, RegisterUtils::ToLocation(x86::rax),
            *returnType, x86::r11)) {
            return;
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
    auto callee = ValidateCall(context, expression);
    if (!callee) {
        return;
    }
    const auto placements = SystemVCallingConvention::GetNativeParameterPlacement(*callee);
    if (!ValidateArguments(context, expression, *callee)) {
        return;
    }
    const CodeAddress target = ResolveAddress(context, *callee);
    size_t scratchSize = 0;
    if (!target || !PlaceArguments(context, expression, *callee, placements, scratchSize)) {
        return;
    }
    EmitCall(context, expression, *callee, target, placements.stackSize + scratchSize, normal, unwind);
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
