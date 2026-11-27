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
#include "../../JitAssert.h"
#include "cangjie/CHIR/IR/Value/Value.h"

#include <algorithm>
#include <vector>

namespace Cangjie::JIT::X86_64 {
namespace {
namespace x86 = asmjit::x86;


const CHIR::Function* ValidateCall(const CHIR::ApplyBase& expression)
{
    auto calleeValue = expression.GetCallee();
    JIT_ASSERT(calleeValue != nullptr && calleeValue->IsFunc(), "apply calls a function");
    auto callee = static_cast<const CHIR::Function*>(calleeValue);
    auto functionType = callee->GetFuncType();
    auto returnType = functionType ? callee->GetReturnType() : nullptr;
    auto result = expression.GetResult();
    // C functions, varargs and mismatched results are unsupported call forms.
    JIT_ASSERT(functionType != nullptr && !functionType->HasVarArg() && !callee->IsCFunc() && returnType &&
        result && result->GetType() && result->GetType()->GetTypeKind() == returnType->GetTypeKind(),
        "apply has a supported callee and result type");
    const bool noResult = returnType->IsUnit() || returnType->IsNothing() || returnType->IsVoid();
    auto returnBase = returnType->IsRef()
        ? static_cast<const CHIR::RefType*>(returnType)->GetBaseType() : nullptr;
    const bool objectReturn = returnBase &&
        (returnBase->IsClass() || returnBase->IsRawArray() || returnBase->IsBox());
    JIT_ASSERT(noResult || TypeUtils::IntegerWidth(*returnType) || returnType->IsBoolean() ||
        objectReturn || returnType->IsCPointer() || returnType->IsCString() || TypeUtils::IsFloating(*returnType),
        "apply return type is a supported scalar or object reference");
    JIT_ASSERT(expression.GetSuccessorIndex(0) == callee->GetParams().size() + 1,
        "apply carries one operand per parameter plus the callee");
    return callee;
}

void ValidateArguments(const CHIR::ApplyBase& expression, const CHIR::Function& callee)
{
    const auto& parameters = callee.GetParams();
    for (size_t i = 0; i < parameters.size(); ++i) {
        auto parameter = parameters[i];
        auto argument = expression.GetOperand(i + 1);
        JIT_ASSERT(TypeUtils::IntegerWidth(*parameter->GetType()) || parameter->GetType()->IsBoolean() ||
            parameter->GetType()->IsRef() || TypeUtils::IsFloating(*parameter->GetType()),
            "parameter type is a supported scalar or reference");
        JIT_ASSERT(argument && argument->GetType() &&
            argument->GetType()->GetTypeKind() == parameter->GetType()->GetTypeKind(),
            "argument type matches the parameter type");
    }
}

CodeAddress ResolveAddress(FunctionContext& context, const CHIR::Function& callee)
{
    auto& global = context.GetGlobalContext();
    if (callee.IsImportedFunc()) {
        auto& bindings = global.GetRuntimeBindings();
        Require(bindings.EnsurePackage(callee.GetPackageName()));
        auto symbol = bindings.Resolve(callee.GetIdentifierWithoutPrefix());
        Require(symbol);
        return reinterpret_cast<CodeAddress>(symbol.Value());
    }
    auto& record = global.GetFunctionRegistry().GetOrCreate(callee);
    return record.GetState() == FunctionState::READY
        ? record.GetCompiledTarget() : CompileFunctionStub::Compile(global, &callee);
}
Location OutgoingLocation(const Location& location)
{
    if (auto stack = location.AsIncomingStack()) {
        return Location::Stack(stack->index, Location::StackBase::Rsp);
    }
    return location;
}

void PlaceTypeInfo(FunctionContext& context, const CHIR::Type* type, const Location& location)
{
    JIT_ASSERT(type != nullptr, "placed type information has a type");
    if (auto reg = location.AsGpRegister()) {
        context.GetTypeInfoEmitter().LoadTypeInfo(*type, RegisterUtils::Get64BitRegister(reg->type));
        return;
    }
    auto stack = location.AsIncomingStack();
    JIT_ASSERT(stack != nullptr, "type information lands in a register or on the stack");
    context.GetTypeInfoEmitter().LoadTypeInfo(*type, x86::rax);
    context.GetAssembler().mov(x86::qword_ptr(x86::rsp, static_cast<int32_t>(stack->index * sizeof(uint64_t))),
        x86::rax);
}

void PlaceStagedTypeInfo(FunctionContext& context, const Location& location, int32_t offset)
{
    auto& assembler = context.GetAssembler();
    if (auto reg = location.AsGpRegister()) {
        assembler.mov(RegisterUtils::Get64BitRegister(reg->type), x86::qword_ptr(x86::rsp, offset));
        return;
    }
    auto stack = location.AsIncomingStack();
    JIT_ASSERT(stack != nullptr, "staged type information lands in a register or on the stack");
    assembler.mov(x86::rax, x86::qword_ptr(x86::rsp, offset));
    assembler.mov(x86::qword_ptr(x86::rsp, static_cast<int32_t>(stack->index * sizeof(uint64_t))), x86::rax);
}

void PrepareIndirectReturn(FunctionContext& context, const CHIR::Value* result, const x86::Gp& target)
{
    auto location = context.GetValueLocation(result);
    auto stack = location->AsStack();
    // TrivialStackRegalloc places every value in a frame-relative slot.
    JIT_ASSERT(stack != nullptr && stack->base == Location::StackBase::Frame,
        "indirect return result occupies a frame slot");
    JIT_ASSERT(result->GetType() != nullptr, "indirect return result has a type");
    MemoryUtils::StoreConstant(context, 0, *location, *result->GetType(), x86::rax);
    context.GetAssembler().lea(target, x86::qword_ptr(x86::rbp,
        StackFrameUtils::GetSlotOffset(stack->index, context.GetRegisterAllocation().GetStackSlotCount())));
}

void PlaceArguments(FunctionContext& context, const CHIR::ApplyBase& expression, const CHIR::Function& callee,
    const ParameterPlacement& placement, size_t& scratchSize)
{
    const auto& typeArgs = expression.GetInstantiatedTypeArgs();
    JIT_ASSERT(typeArgs.size() == placement.genericTypeInfoLocations.size(),
        "instantiated type arguments match the generic placement");
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
        scratchSize = static_cast<uint32_t>((types.size() * sizeof(uint64_t) + 15) & ~size_t{15});
        context.GetAssembler().sub(x86::rsp, scratchSize);
        context.GetTypeInfoEmitter().StoreTypeInfos(types, 0);
    }
    context.GetAssembler().sub(x86::rsp, placement.stackSize);
    const auto& parameters = callee.GetParams();
    for (size_t i = 0; i < parameters.size(); ++i) {
        const auto location = placement.locations.find(parameters[i]);
        JIT_ASSERT(location != placement.locations.end(), "ABI placement covers every parameter");
        auto stack = location->second.AsIncomingStack();
        if (!stack) {
            continue;
        }
        auto value = expression.GetOperand(i + 1);
        auto source = context.GetValueLocation(value);
        JIT_ASSERT(value->GetType() != nullptr, "stack argument has a type");
        MemoryUtils::Move(context, Location::Stack(stack->index, Location::StackBase::Rsp), *source,
            *value->GetType(), x86::rax);
    }
    for (size_t i = 0; i < parameters.size(); ++i) {
        const auto it = placement.locations.find(parameters[i]);
        JIT_ASSERT(it != placement.locations.end(), "ABI placement covers every parameter");
        const auto& location = it->second;
        if (location.AsIncomingStack()) {
            continue;
        }
        auto value = expression.GetOperand(i + 1);
        auto source = context.GetValueLocation(value);
        JIT_ASSERT(value->GetType() != nullptr, "register argument has a type");
        MemoryUtils::Move(context, OutgoingLocation(location), *source, *value->GetType(), x86::r11);
    }
    size_t typeIndex = 0;
    const auto placeType = [&](const CHIR::Type* type, const Location& location) {
        if (!needsScratch) {
            PlaceTypeInfo(context, type, location);
            return;
        }
        JIT_ASSERT(type != nullptr, "staged type information has a type");
        const auto offset = static_cast<int32_t>(placement.stackSize + typeIndex++ * sizeof(uint64_t));
        PlaceStagedTypeInfo(context, location, offset);
    };
    for (size_t i = 0; i < typeArgs.size(); ++i) {
        placeType(typeArgs[i], placement.genericTypeInfoLocations[i]);
    }
    if (placement.outerTypeInfoLocation) {
        placeType(outer, *placement.outerTypeInfoLocation);
    }
    if (placement.thisTypeInfoLocation) {
        placeType(thisType, *placement.thisTypeInfoLocation);
    }
    auto returnType = callee.GetReturnType();
    if (returnType->IsUnit() || returnType->IsNothing()) {
        PrepareIndirectReturn(context, expression.GetResult(), x86::rdi);
    }
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
    context.GetSafepointRecorder().Record(expression);
    if (!callee.IsImportedFunc()) {
        auto& record = context.GetGlobalContext().GetFunctionRegistry().GetOrCreate(callee);
        if (target == record.GetEntryStub()) {
            // mov r11, imm64 (10 bytes) plus call r11 (3 bytes).
            JIT_ASSERT(assembler.offset() - instructionOffset == 13,
                "lazy call site uses the patchable 13-byte encoding");
            context.GetLazyCallSites().push_back({instructionOffset + 2, assembler.offset(), &record});
        }
    }
    assembler.add(x86::rsp, stackSize);
    auto returnType = callee.GetReturnType();
    if (!returnType->IsUnit() && !returnType->IsNothing() && !returnType->IsVoid()) {
        auto resultLocation = context.GetValueLocation(expression.GetResult());
        if (TypeUtils::IsFloating(*returnType)) {
            MemoryUtils::Move(context, *resultLocation, RegisterUtils::ToLocation(x86::xmm0),
                *returnType, x86::rax);
        } else {
            MemoryUtils::Move(context, *resultLocation, RegisterUtils::ToLocation(x86::rax),
                *returnType, x86::r11);
        }
    }
    if (normal && unwind) {
        assembler.jmp(context.GetBlockLabel(normal));
        context.GetExceptionLandingEmitter().Emit(*unwind, callStart, callEnd);
    }
}


void CompileCall(FunctionContext& context, const CHIR::ApplyBase& expression,
    const CHIR::Block* normal, const CHIR::Block* unwind)
{
    const auto callee = ValidateCall(expression);
    const auto placements = SystemVCallingConvention::GetNativeParameterPlacement(*callee);
    ValidateArguments(expression, *callee);
    const CodeAddress target = ResolveAddress(context, *callee);
    JIT_ASSERT(target != 0, "call resolves to a target");
    size_t scratchSize = 0;
    PlaceArguments(context, expression, *callee, placements, scratchSize);
    EmitCall(context, expression, *callee, target, placements.stackSize + scratchSize, normal, unwind);
}
} // namespace

void CompileApply::Compile(FunctionContext& context, const CHIR::Apply& expression)
{
    CompileCall(context, expression, nullptr, nullptr);
}

void CompileApply::Compile(FunctionContext& context, const CHIR::TryApply& expression)
{
    JIT_ASSERT(expression.GetSuccessBlock() && expression.GetErrorBlock(),
        "try apply has success and error destinations");
    CompileCall(context, expression, expression.GetSuccessBlock(), expression.GetErrorBlock());
}
} // namespace Cangjie::JIT::X86_64
