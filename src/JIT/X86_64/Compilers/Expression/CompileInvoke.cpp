// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "CompileInvoke.h"

#include <algorithm>
#include <limits>
#include <vector>

#include "../Context/FunctionContext.h"
#include "../Context/GlobalContext.h"
#include "../../Utils/MemoryUtils.h"
#include "../../Utils/TypeUtils.h"
#include "../../Utils/RegisterUtils.h"
#include "../../Utils/StackFrameUtils.h"
#include "../../Utils/SystemVCallingConvention.h"
#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Expression/Terminator.h"
#include "cangjie/CHIR/IR/Type/ClassDef.h"
#include "cangjie/CHIR/IR/Type/Type.h"
#include "cangjie/CHIR/IR/Value/Value.h"
#include "../../JitAssert.h"
#include "cangjie/CHIR/Utils/Utils.h"

namespace Cangjie::JIT::X86_64 {
namespace {
namespace x86 = asmjit::x86;

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


void EmitTarget(FunctionContext& context, const CHIR::DynamicDispatch& expression,
    const CHIR::Value* receiver, bool isStatic)
{
    auto& a = context.GetAssembler();
    auto receiverType = receiver ? receiver->GetType() : nullptr;
    const CHIR::Type* objectType = receiverType && receiverType->IsRef()
        ? static_cast<const CHIR::RefType*>(receiverType)->GetBaseType() : nullptr;
    const bool autoEnv = !isStatic && objectType && objectType->IsAutoEnv();
    auto receiverLocation = context.GetValueLocation(receiver);
    JIT_ASSERT(receiverType != nullptr, "invoke has a receiver type");
    MemoryUtils::Move(context, RegisterUtils::ToLocation(x86::rax), *receiverLocation,
        *receiverType, x86::r11);
    if (!isStatic) {
        JIT_ASSERT(objectType != nullptr && objectType->IsClass(),
            "virtual invoke receiver is a class reference");
        if (autoEnv) {
            const auto index = CHIR::GetMethodIdxInAutoEnvObject(expression.GetMethodName());
            a.mov(x86::r11, x86::qword_ptr(x86::rax, static_cast<int32_t>(8 + index * sizeof(uint64_t))));
            a.mov(x86::qword_ptr(x86::rsp), x86::r11);
        }
        a.mov(x86::rax, x86::qword_ptr(x86::rax));
        a.shl(x86::rax, 16);
        a.shr(x86::rax, 16);
    }
    a.mov(x86::qword_ptr(x86::rsp, 16), x86::rax);
    if (autoEnv) {
        a.mov(x86::qword_ptr(x86::rsp, 8), x86::rax);
        return;
    }

    auto& bindings = context.GetGlobalContext().GetRuntimeBindings();
    auto intro = expression.GetInstSrcParentCustomTypeOfMethod(context.GetGlobalContext().GetCHIRBuilder());
    JIT_ASSERT(intro != nullptr, "invoke has a defining class");
    const auto def = intro->GetClassDef();
    JIT_ASSERT(def != nullptr, "invoke has a defining class");
    context.GetTypeInfoEmitter().LoadTypeInfo(*intro, x86::rax);
    a.mov(x86::qword_ptr(x86::rsp, 24), x86::rax);
    auto outerLookup = bindings.GetMethodOuterTypeInfo();
    const auto index = expression.GetVirtualMethodOffset();
    if (def->IsInterface()) {
        auto mtable = bindings.GetMethodTable();
        a.mov(x86::rdi, x86::qword_ptr(x86::rsp, 16));
        a.mov(x86::rsi, x86::qword_ptr(x86::rsp, 24));
        a.long_().mov(x86::r11, asmjit::imm(reinterpret_cast<uintptr_t>(mtable)));
        a.call(x86::r11);
        a.mov(x86::rax, x86::qword_ptr(x86::rax, static_cast<int32_t>(index * sizeof(uint64_t))));
    } else {
        size_t depth = 0;
        for (auto current = def; current; current = current->GetSuperClassDef()) {
            ++depth;
        }
        auto update = bindings.GetUpdateVTable();
        // x86-64 native TypeInfo.vExtensionDataStart and ExtensionData.funcTable.
        constexpr int32_t VTABLE_OFFSET = 72;
        constexpr int32_t FUNCTION_TABLE_OFFSET = 32;
        a.mov(x86::rdi, x86::qword_ptr(x86::rsp, 16));
        a.mov(x86::rdx, x86::qword_ptr(x86::rdi, VTABLE_OFFSET));
        a.mov(x86::rdx, x86::qword_ptr(x86::rdx, static_cast<int32_t>((depth - 1) * sizeof(uint64_t))));
        a.mov(x86::qword_ptr(x86::rsp, 32), x86::rdx);
        a.mov(x86::rsi, x86::qword_ptr(x86::rsp, 24));
        a.long_().mov(x86::r11, asmjit::imm(reinterpret_cast<uintptr_t>(update)));
        a.call(x86::r11);
        a.mov(x86::rax, x86::qword_ptr(x86::rsp, 32));
        a.mov(x86::rax, x86::qword_ptr(x86::rax, FUNCTION_TABLE_OFFSET));
        a.mov(x86::rax, x86::qword_ptr(x86::rax, static_cast<int32_t>(index * sizeof(uint64_t))));
    }
    a.mov(x86::qword_ptr(x86::rsp), x86::rax);
    a.mov(x86::rdi, x86::qword_ptr(x86::rsp, 16));
    a.mov(x86::rsi, x86::qword_ptr(x86::rsp, 24));
    a.long_().mov(x86::rdx, asmjit::imm(index));
    a.long_().mov(x86::r11, asmjit::imm(reinterpret_cast<uintptr_t>(outerLookup)));
    a.call(x86::r11);
    a.mov(x86::qword_ptr(x86::rsp, 8), x86::rax);
}

void PlaceArguments(FunctionContext& context, const CHIR::DynamicDispatch& expression,
    const ParameterPlacement& placement, bool needsTypeScratch, uint32_t typeScratchSize)
{
    const auto args = expression.GetArgs();
    const auto& parameters = expression.GetCallee()->GetParams();
    const auto& typeArgs = expression.GetInstantiatedTypeArgs();
    for (size_t i = 0; i < args.size(); ++i) {
        const auto it = placement.locations.find(parameters[i]);
        JIT_ASSERT(it != placement.locations.end() && args[i] && args[i]->GetType(),
            "ABI placement covers every argument");
        auto stack = it->second.AsIncomingStack();
        if (!stack) {
            continue;
        }
        auto source = context.GetValueLocation(args[i]);
        MemoryUtils::Move(context, Location::Stack(stack->index, Location::StackBase::Rsp),
            *source, *args[i]->GetType(), x86::rax);
    }
    for (size_t i = 0; i < args.size(); ++i) {
        const auto it = placement.locations.find(parameters[i]);
        JIT_ASSERT(it != placement.locations.end(), "ABI placement covers every argument");
        if (it->second.AsIncomingStack()) {
            continue;
        }
        auto source = context.GetValueLocation(args[i]);
        MemoryUtils::Move(context, OutgoingLocation(it->second), *source,
            *args[i]->GetType(), x86::r11);
    }
    if (needsTypeScratch) {
        for (size_t i = 0; i < typeArgs.size(); ++i) {
            PlaceStagedTypeInfo(context, placement.genericTypeInfoLocations[i],
                static_cast<int32_t>(placement.stackSize + i * sizeof(uint64_t)));
        }
    } else {
        for (size_t i = 0; i < typeArgs.size(); ++i) {
            PlaceTypeInfo(context, typeArgs[i], placement.genericTypeInfoLocations[i]);
        }
    }
    const auto scratch = static_cast<int32_t>(placement.stackSize + typeScratchSize);
    if (placement.outerTypeInfoLocation) {
        PlaceStagedTypeInfo(context, *placement.outerTypeInfoLocation, scratch + 8);
    }
    if (placement.thisTypeInfoLocation) {
        PlaceStagedTypeInfo(context, *placement.thisTypeInfoLocation, scratch + 16);
    }
}

void CompileCall(FunctionContext& context, const CHIR::DynamicDispatch& expression,
    const CHIR::Value* receiver, bool isStatic, const CHIR::Block* normal, const CHIR::Block* unwind)
{
    auto callee = expression.GetCallee();
    auto returnType = expression.GetMethodType() ? expression.GetMethodType()->GetReturnType() : nullptr;
    auto result = expression.GetResult();
    JIT_ASSERT(callee && returnType && result && result->GetType(),
        "invoke has a callee, a return type and a result");
    const auto placement = SystemVCallingConvention::GetVirtualParameterPlacement(expression, isStatic);
    JIT_ASSERT(placement.basePointerLocations.empty() &&
        (!placement.indirectReturn || returnType->IsUnit() || returnType->IsNothing()),
        "invoke uses a supported calling convention");
    const auto args = expression.GetArgs();
    const auto& parameters = callee->GetParams();
    JIT_ASSERT(args.size() == parameters.size(), "invoke argument count matches the callee parameters");
    const auto& typeArgs = expression.GetInstantiatedTypeArgs();
    JIT_ASSERT(typeArgs.size() == placement.genericTypeInfoLocations.size(),
        "invoke type arguments match the generic placement");
    auto& assembler = context.GetAssembler();
    constexpr uint32_t SCRATCH_SIZE = 48;
    assembler.sub(x86::rsp, SCRATCH_SIZE);
    EmitTarget(context, expression, receiver, isStatic);
    const bool needsTypeScratch = std::any_of(typeArgs.begin(), typeArgs.end(), [](const CHIR::Type* type) {
        return type && !type->IsGeneric() && !type->IsThis() && type->IsGenericRelated();
    });
    uint32_t typeScratchSize = 0;
    if (needsTypeScratch) {
        const std::vector<const CHIR::Type*> types(typeArgs.begin(), typeArgs.end());
        JIT_ASSERT(types.size() <= (static_cast<size_t>(std::numeric_limits<int32_t>::max()) - 15) / sizeof(uint64_t),
            "staged type information fits the scratch area");
        typeScratchSize = static_cast<uint32_t>((types.size() * sizeof(uint64_t) + 15) & ~size_t{15});
        assembler.sub(x86::rsp, typeScratchSize);
        context.GetTypeInfoEmitter().StoreTypeInfos(types, 0);
    }
    assembler.sub(x86::rsp, placement.stackSize);
    PlaceArguments(context, expression, placement, needsTypeScratch, typeScratchSize);
    if (placement.indirectReturn) {
        PrepareIndirectReturn(context, result, x86::rdi);
    }
    const auto scratch = static_cast<int32_t>(placement.stackSize + typeScratchSize);
    assembler.mov(x86::r11, x86::qword_ptr(x86::rsp, scratch));
    const auto callStart = assembler.offset();
    assembler.call(x86::r11);
    const auto callEnd = assembler.offset();
    context.GetSafepointRecorder().Record(expression);
    assembler.add(x86::rsp, placement.stackSize + typeScratchSize + SCRATCH_SIZE);
    if (!returnType->IsUnit() && !returnType->IsNothing() && !returnType->IsVoid()) {
        const auto width = TypeUtils::TypeWidth(*returnType);
        // Unit results travel indirectly and are handled before the call.
        JIT_ASSERT(width || returnType->IsRef() || returnType->IsCPointer() || returnType->IsCString(),
            "invoke return type is supported");
        auto resultLocation = context.GetValueLocation(result);
        const auto source = TypeUtils::IsFloating(*returnType)
            ? RegisterUtils::ToLocation(x86::xmm0) : RegisterUtils::ToLocation(x86::rax);
        MemoryUtils::Move(context, *resultLocation, source, *returnType, x86::r11);
    }
    if (normal && unwind) {
        assembler.jmp(context.GetBlockLabel(normal));
        context.GetExceptionLandingEmitter().Emit(*unwind, callStart, callEnd);
    }
}
} // namespace

void CompileInvoke::Compile(FunctionContext& context, const CHIR::Invoke& expression)
{
    CompileCall(context, expression, expression.GetObject(), false, nullptr, nullptr);
}

void CompileInvoke::Compile(FunctionContext& context, const CHIR::TryInvoke& expression)
{
    JIT_ASSERT(expression.GetSuccessBlock() && expression.GetErrorBlock(),
        "try invoke has success and error destinations");
    CompileCall(context, expression, expression.GetObject(), false, expression.GetSuccessBlock(), expression.GetErrorBlock());
}

void CompileInvoke::Compile(FunctionContext& context, const CHIR::InvokeStatic& expression)
{
    CompileCall(context, expression, expression.GetRTTIValue(), true, nullptr, nullptr);
}

void CompileInvoke::Compile(FunctionContext& context, const CHIR::TryInvokeStatic& expression)
{
    JIT_ASSERT(expression.GetSuccessBlock() && expression.GetErrorBlock(),
        "try invoke has success and error destinations");
    CompileCall(context, expression, expression.GetRTTIValue(), true, expression.GetSuccessBlock(), expression.GetErrorBlock());
}
} // namespace Cangjie::JIT::X86_64
