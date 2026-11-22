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
#include "../../Utils/RegisterUtils.h"
#include "../../Utils/SystemVCallingConvention.h"
#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Expression/Terminator.h"
#include "cangjie/CHIR/IR/Type/ClassDef.h"
#include "cangjie/CHIR/IR/Type/Type.h"
#include "cangjie/CHIR/IR/Value/Value.h"
#include "cangjie/CHIR/Utils/Utils.h"

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


bool EmitTarget(FunctionContext& context, const CHIR::DynamicDispatch& expression,
    const CHIR::Value* receiver, bool isStatic)
{
    auto& a = context.GetAssembler();
    const auto* receiverType = receiver ? receiver->GetType() : nullptr;
    const CHIR::Type* objectType = receiverType && receiverType->IsRef()
        ? static_cast<const CHIR::RefType*>(receiverType)->GetBaseType() : nullptr;
    const bool autoEnv = !isStatic && objectType && objectType->IsAutoEnv();
    if (!MemoryUtils::LoadValue(context, receiver, x86::rax)) {
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
    const auto* intro = expression.GetInstSrcParentCustomTypeOfMethod(context.GetGlobalContext().GetCHIRBuilder());
    const auto* def = intro ? intro->GetClassDef() : nullptr;
    if (!def || !context.GetTypeInfoEmitter().LoadTypeInfo(*intro, x86::rax)) {
        return false;
    }
    a.mov(x86::qword_ptr(x86::rsp, 24), x86::rax);
    auto* outerLookup = Resolve(context, bindings.GetMethodOuterTypeInfo());
    if (!outerLookup) {
        return false;
    }
    const auto index = expression.GetVirtualMethodOffset();
    if (index > static_cast<size_t>(std::numeric_limits<int32_t>::max()) / sizeof(uint64_t)) {
        context.Fail(JitFailureKind::UnsupportedCall, expression.GetExprKind());
        return false;
    }
    if (def->IsInterface()) {
        auto* mtable = Resolve(context, bindings.GetMethodTable());
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
        for (auto* current = def; current; current = current->GetSuperClassDef()) {
            ++depth;
        }
        if (depth == 0 || depth - 1 > static_cast<size_t>(std::numeric_limits<int32_t>::max()) / sizeof(uint64_t)) {
            context.Fail(JitFailureKind::UnsupportedCall, expression.GetExprKind());
            return false;
        }
        auto* update = Resolve(context, bindings.GetUpdateVTable());
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

void CompileCall(FunctionContext& context, const CHIR::DynamicDispatch& expression,
    const CHIR::Value* receiver, bool isStatic, const CHIR::Block* normal, const CHIR::Block* unwind)
{
    const auto* callee = expression.GetCallee();
    const auto* returnType = expression.GetMethodType() ? expression.GetMethodType()->GetReturnType() : nullptr;
    const auto* result = expression.GetResult();
    auto placement = SystemVCallingConvention::GetVirtualParameterPlacement(expression, isStatic);
    if (!callee || !returnType || !result || !result->GetType() || !placement ||
        !placement->basePointerLocations.empty() ||
        (placement->indirectReturn && !returnType->IsUnit() && !returnType->IsNothing())) {
        context.Fail(JitFailureKind::UnsupportedCallingConvention, expression.GetExprKind());
        return;
    }
    const auto args = expression.GetArgs();
    const auto& parameters = callee->GetParams();
    if (args.size() != parameters.size() ||
        placement->stackSize > static_cast<uint32_t>(std::numeric_limits<int32_t>::max() - 48)) {
        context.Fail(JitFailureKind::UnsupportedCall, expression.GetExprKind());
        return;
    }
    const auto& typeArgs = expression.GetInstantiatedTypeArgs();
    if (typeArgs.size() != placement->genericTypeInfoLocations.size()) {
        context.Fail(JitFailureKind::UnsupportedCallingConvention, expression.GetExprKind());
        return;
    }
    auto& a = context.GetAssembler();
    constexpr uint32_t SCRATCH_SIZE = 48;
    MemoryUtils::ReserveStack(context, SCRATCH_SIZE);
    if (!EmitTarget(context, expression, receiver, isStatic)) {
        return;
    }
    const bool needsTypeScratch = std::any_of(typeArgs.begin(), typeArgs.end(), [](const CHIR::Type* type) {
        return type && !type->IsGeneric() && !type->IsThis() && type->IsGenericRelated();
    });
    uint32_t typeScratchSize = 0;
    if (needsTypeScratch) {
        const std::vector<const CHIR::Type*> types(typeArgs.begin(), typeArgs.end());
        const auto size = MemoryUtils::StageTypeInfos(context, types);
        if (!size || *size > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) -
                placement->stackSize - SCRATCH_SIZE) {
            context.Fail(JitFailureKind::UnsupportedCallingConvention, expression.GetExprKind());
            return;
        }
        typeScratchSize = *size;
    }
    MemoryUtils::ReserveStack(context, placement->stackSize);
    for (size_t i = 0; i < args.size(); ++i) {
        const auto it = placement->locations.find(parameters[i]);
        if (it == placement->locations.end() || !args[i] || !args[i]->GetType()) {
            context.Fail(JitFailureKind::UnsupportedCall, expression.GetExprKind());
            return;
        }
        if (const auto* stack = it->second.AsIncomingStack()) {
            if (!MemoryUtils::StoreOutgoingArgument(context, args[i], stack->index, placement->stackSize)) {
                return;
            }
        }
    }
    for (size_t i = 0; i < args.size(); ++i) {
        const auto& location = placement->locations.at(parameters[i]);
        if (const auto* reg = location.AsRegister()) {
            const auto target = RegisterUtils::Get64BitRegister(reg->type);
            if (!target || !MemoryUtils::LoadValue(context, args[i], *target)) {
                context.Fail(JitFailureKind::UnsupportedCall, expression.GetExprKind());
                return;
            }
        } else if (const auto* simd = location.AsSimdRegister()) {
            const auto target = RegisterUtils::GetArgumentSimdRegister(simd->index);
            if (!target || !MemoryUtils::LoadFloatValue(context, args[i], *target, x86::rax)) {
                context.Fail(JitFailureKind::UnsupportedCall, expression.GetExprKind());
                return;
            }
        }
    }
    for (size_t i = 0; i < typeArgs.size(); ++i) {
        const bool placed = needsTypeScratch
            ? MemoryUtils::PlaceStagedTypeInfo(context, placement->genericTypeInfoLocations[i], placement->stackSize,
                static_cast<int32_t>(placement->stackSize + i * sizeof(uint64_t)))
            : MemoryUtils::PlaceTypeInfo(context, typeArgs[i], placement->genericTypeInfoLocations[i],
                placement->stackSize);
        if (!placed) {
            return;
        }
    }
    const auto scratch = static_cast<int32_t>(placement->stackSize + typeScratchSize);
    if (placement->outerTypeInfoLocation &&
        !MemoryUtils::PlaceStagedTypeInfo(context, *placement->outerTypeInfoLocation, placement->stackSize, scratch + 8)) {
        return;
    }
    if (placement->thisTypeInfoLocation &&
        !MemoryUtils::PlaceStagedTypeInfo(context, *placement->thisTypeInfoLocation, placement->stackSize, scratch + 16)) {
        return;
    }
    if (placement->indirectReturn && !MemoryUtils::ZeroStackValueAndLoadAddress(context, result, x86::rdi)) {
        return;
    }
    a.mov(x86::r11, x86::qword_ptr(x86::rsp, scratch));
    const auto callStart = a.offset();
    a.call(x86::r11);
    const auto callEnd = a.offset();
    if (!context.GetSafepointRecorder().Record(expression)) {
        return;
    }
    MemoryUtils::ReleaseStack(context, placement->stackSize + typeScratchSize + SCRATCH_SIZE);
    if (!returnType->IsUnit() && !returnType->IsNothing() && !returnType->IsVoid()) {
        if (returnType->GetTypeKind() == CHIR::Type::TYPE_FLOAT32 ||
            returnType->GetTypeKind() == CHIR::Type::TYPE_FLOAT64) {
            if (!MemoryUtils::StoreFloatValue(context, result, x86::xmm0, x86::rax)) {
                return;
            }
        } else {
            const auto width = returnType->IsBoolean() ? 8U : RegisterUtils::IntegerWidth(*returnType);
            if (width) {
                RegisterUtils::NormalizeInteger(a, x86::rax, width, returnType->IsSignedInteger());
            } else if (!returnType->IsRef() && !returnType->IsCPointer() && !returnType->IsCString()) {
                context.Fail(JitFailureKind::UnsupportedReturnType, expression.GetExprKind());
                return;
            }
            if (!MemoryUtils::StoreValue(context, result, x86::rax)) {
                return;
            }
        }
    }
    if (normal && unwind && !context.GetFailure()) {
        a.jmp(context.GetLocalFinalizers().GetEdgeLabel(normal));
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
