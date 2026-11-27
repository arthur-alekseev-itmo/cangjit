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
#include "cangjie/CHIR/Utils/Utils.h"
#include "cangjie/Utils/CheckUtils.h"

namespace Cangjie::JIT::X86_64 {
namespace {
namespace x86 = asmjit::x86;

void* Resolve(FunctionContext& context, SymbolResult result)
{
    if (!result) {
        context.Fail(JitFailureKind::UnresolvedSymbol);
        return nullptr;
    }
    return result.Value();
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


bool EmitTarget(FunctionContext& context, const CHIR::DynamicDispatch& expression,
    const CHIR::Value* receiver, bool isStatic)
{
    auto& a = context.GetAssembler();
    auto receiverType = receiver ? receiver->GetType() : nullptr;
    const CHIR::Type* objectType = receiverType && receiverType->IsRef()
        ? static_cast<const CHIR::RefType*>(receiverType)->GetBaseType() : nullptr;
    const bool autoEnv = !isStatic && objectType && objectType->IsAutoEnv();
    auto receiverLocation = context.GetValueLocation(receiver);
    if (!receiverType ||
        !MemoryUtils::Move(context, RegisterUtils::ToLocation(x86::rax), *receiverLocation,
            *receiverType, x86::r11)) {
        return false;
    }
    if (!isStatic) {
        if (!objectType || !objectType->IsClass()) {
            context.Fail(JitFailureKind::UnsupportedCall, expression.GetExprKind());
            return false;
        }
        if (autoEnv) {
            const auto index = CHIR::GetMethodIdxInAutoEnvObject(expression.GetMethodName());
            if (index > (static_cast<size_t>(std::numeric_limits<int32_t>::max()) - 8) / sizeof(uint64_t)) {
                context.Fail(JitFailureKind::UnsupportedCall, expression.GetExprKind());
                return false;
            }
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
        return true;
    }

    auto& bindings = context.GetGlobalContext().GetRuntimeBindings();
    auto intro = expression.GetInstSrcParentCustomTypeOfMethod(context.GetGlobalContext().GetCHIRBuilder());
    auto def = intro ? intro->GetClassDef() : nullptr;
    if (!def || !context.GetTypeInfoEmitter().LoadTypeInfo(*intro, x86::rax)) {
        return false;
    }
    a.mov(x86::qword_ptr(x86::rsp, 24), x86::rax);
    auto outerLookup = Resolve(context, bindings.GetMethodOuterTypeInfo());
    if (!outerLookup) {
        return false;
    }
    const auto index = expression.GetVirtualMethodOffset();
    if (index > static_cast<size_t>(std::numeric_limits<int32_t>::max()) / sizeof(uint64_t)) {
        context.Fail(JitFailureKind::UnsupportedCall, expression.GetExprKind());
        return false;
    }
    if (def->IsInterface()) {
        auto mtable = Resolve(context, bindings.GetMethodTable());
        if (!mtable) {
            return false;
        }
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
        if (depth - 1 > static_cast<size_t>(std::numeric_limits<int32_t>::max()) / sizeof(uint64_t)) {
            context.Fail(JitFailureKind::UnsupportedCall, expression.GetExprKind());
            return false;
        }
        auto update = Resolve(context, bindings.GetUpdateVTable());
        if (!update) {
            return false;
        }
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
    return true;
}

bool PlaceArguments(FunctionContext& context, const CHIR::DynamicDispatch& expression,
    const ParameterPlacement& placement, bool needsTypeScratch, uint32_t typeScratchSize)
{
    const auto args = expression.GetArgs();
    const auto& parameters = expression.GetCallee()->GetParams();
    const auto& typeArgs = expression.GetInstantiatedTypeArgs();
    for (size_t i = 0; i < args.size(); ++i) {
        const auto it = placement.locations.find(parameters[i]);
        CJC_ASSERT(it != placement.locations.end());
        if (it == placement.locations.end() || !args[i] || !args[i]->GetType()) {
            context.Fail(JitFailureKind::UnsupportedCall, expression.GetExprKind());
            return false;
        }
        auto stack = it->second.AsIncomingStack();
        if (!stack) {
            continue;
        }
        auto source = context.GetValueLocation(args[i]);
        if (!MemoryUtils::Move(context, Location::Stack(stack->index, Location::StackBase::Rsp),
            *source, *args[i]->GetType(), x86::rax)) {
            return false;
        }
    }
    for (size_t i = 0; i < args.size(); ++i) {
        const auto it = placement.locations.find(parameters[i]);
        CJC_ASSERT(it != placement.locations.end());
        if (it == placement.locations.end()) {
            context.Fail(JitFailureKind::UnsupportedCall, expression.GetExprKind());
            return false;
        }
        if (it->second.AsIncomingStack()) {
            continue;
        }
        auto source = context.GetValueLocation(args[i]);
        if (!MemoryUtils::Move(context, OutgoingLocation(it->second), *source,
            *args[i]->GetType(), x86::r11)) {
            return false;
        }
    }
    for (size_t i = 0; i < typeArgs.size(); ++i) {
        const auto& location = placement.genericTypeInfoLocations[i];
        const auto placed = needsTypeScratch
            ? PlaceStagedTypeInfo(context, location,
                static_cast<int32_t>(placement.stackSize + i * sizeof(uint64_t)))
            : PlaceTypeInfo(context, typeArgs[i], location);
        if (!placed) {
            return false;
        }
    }
    const auto scratch = static_cast<int32_t>(placement.stackSize + typeScratchSize);
    if (placement.outerTypeInfoLocation &&
        !PlaceStagedTypeInfo(context, *placement.outerTypeInfoLocation, scratch + 8)) {
        return false;
    }
    if (placement.thisTypeInfoLocation &&
        !PlaceStagedTypeInfo(context, *placement.thisTypeInfoLocation, scratch + 16)) {
        return false;
    }
    return true;
}

void CompileCall(FunctionContext& context, const CHIR::DynamicDispatch& expression,
    const CHIR::Value* receiver, bool isStatic, const CHIR::Block* normal, const CHIR::Block* unwind)
{
    auto callee = expression.GetCallee();
    auto returnType = expression.GetMethodType() ? expression.GetMethodType()->GetReturnType() : nullptr;
    auto result = expression.GetResult();
    if (!callee || !returnType || !result || !result->GetType()) {
        context.Fail(JitFailureKind::UnsupportedCallingConvention, expression.GetExprKind());
        return;
    }
    const auto placement = SystemVCallingConvention::GetVirtualParameterPlacement(expression, isStatic);
    if (!placement.basePointerLocations.empty() ||
        (placement.indirectReturn && !returnType->IsUnit() && !returnType->IsNothing())) {
        context.Fail(JitFailureKind::UnsupportedCallingConvention, expression.GetExprKind());
        return;
    }
    const auto args = expression.GetArgs();
    const auto& parameters = callee->GetParams();
    if (args.size() != parameters.size() ||
        placement.stackSize > static_cast<uint32_t>(std::numeric_limits<int32_t>::max() - 48)) {
        context.Fail(JitFailureKind::UnsupportedCall, expression.GetExprKind());
        return;
    }
    const auto& typeArgs = expression.GetInstantiatedTypeArgs();
    if (typeArgs.size() != placement.genericTypeInfoLocations.size()) {
        context.Fail(JitFailureKind::UnsupportedCallingConvention, expression.GetExprKind());
        return;
    }
    auto& assembler = context.GetAssembler();
    constexpr uint32_t SCRATCH_SIZE = 48;
    assembler.sub(x86::rsp, SCRATCH_SIZE);
    if (!EmitTarget(context, expression, receiver, isStatic)) {
        return;
    }
    const bool needsTypeScratch = std::any_of(typeArgs.begin(), typeArgs.end(), [](const CHIR::Type* type) {
        return type && !type->IsGeneric() && !type->IsThis() && type->IsGenericRelated();
    });
    uint32_t typeScratchSize = 0;
    if (needsTypeScratch) {
        const std::vector<const CHIR::Type*> types(typeArgs.begin(), typeArgs.end());
        if (types.size() > (static_cast<size_t>(std::numeric_limits<int32_t>::max()) - 15) / sizeof(uint64_t)) {
            context.Fail(JitFailureKind::UnsupportedCallingConvention, expression.GetExprKind());
            return;
        }
        typeScratchSize = static_cast<uint32_t>((types.size() * sizeof(uint64_t) + 15) & ~size_t{15});
        if (typeScratchSize > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) -
                placement.stackSize - SCRATCH_SIZE) {
            context.Fail(JitFailureKind::UnsupportedCallingConvention, expression.GetExprKind());
            return;
        }
        assembler.sub(x86::rsp, typeScratchSize);
        if (!context.GetTypeInfoEmitter().StoreTypeInfos(types, 0)) {
            return;
        }
    }
    assembler.sub(x86::rsp, placement.stackSize);
    if (!PlaceArguments(context, expression, placement, needsTypeScratch, typeScratchSize)) {
        return;
    }
    if (placement.indirectReturn && !PrepareIndirectReturn(context, result, x86::rdi)) {
        return;
    }
    const auto scratch = static_cast<int32_t>(placement.stackSize + typeScratchSize);
    assembler.mov(x86::r11, x86::qword_ptr(x86::rsp, scratch));
    const auto callStart = assembler.offset();
    assembler.call(x86::r11);
    const auto callEnd = assembler.offset();
    if (!context.GetSafepointRecorder().Record(expression)) {
        return;
    }
    assembler.add(x86::rsp, placement.stackSize + typeScratchSize + SCRATCH_SIZE);
    if (!returnType->IsUnit() && !returnType->IsNothing() && !returnType->IsVoid()) {
        const auto width = TypeUtils::TypeWidth(*returnType);
        if (!width && !returnType->IsRef() && !returnType->IsCPointer() && !returnType->IsCString()) {
            context.Fail(JitFailureKind::UnsupportedReturnType, expression.GetExprKind());
            return;
        }
        auto resultLocation = context.GetValueLocation(result);
        const auto source = TypeUtils::IsFloating(*returnType)
            ? RegisterUtils::ToLocation(x86::xmm0) : RegisterUtils::ToLocation(x86::rax);
        if (!MemoryUtils::Move(context, *resultLocation, source, *returnType, x86::r11)) {
            return;
        }
    }
    if (normal && unwind && !context.GetFailure()) {
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
    if (!expression.GetSuccessBlock() || !expression.GetErrorBlock()) {
        context.Fail(JitFailureKind::MissingBranchDestination);
        return;
    }
    CompileCall(context, expression, expression.GetObject(), false, expression.GetSuccessBlock(), expression.GetErrorBlock());
}

void CompileInvoke::Compile(FunctionContext& context, const CHIR::InvokeStatic& expression)
{
    CompileCall(context, expression, expression.GetRTTIValue(), true, nullptr, nullptr);
}

void CompileInvoke::Compile(FunctionContext& context, const CHIR::TryInvokeStatic& expression)
{
    if (!expression.GetSuccessBlock() || !expression.GetErrorBlock()) {
        context.Fail(JitFailureKind::MissingBranchDestination);
        return;
    }
    CompileCall(context, expression, expression.GetRTTIValue(), true, expression.GetSuccessBlock(), expression.GetErrorBlock());
}
} // namespace Cangjie::JIT::X86_64
