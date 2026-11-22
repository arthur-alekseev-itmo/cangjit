// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "CompileObject.h"

#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "../Context/GlobalContext.h"
#include "../Function/CompileFunctionStub.h"
#include "../../Utils/MemoryUtils.h"
#include "cangjie/CHIR/IR/Type/ClassDef.h"
#include "cangjie/CHIR/IR/Value/Value.h"
#include "cangjie/Utils/ConstantsUtils.h"

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

struct AllocationFunction {
    SymbolResult address;
    bool pinned;
};

AllocationFunction SelectAllocationFunction(JitRuntimeBindings& bindings, const CHIR::ClassDef& def, bool local)
{
    if (def.GetFinalizer()) {
        return {local ? bindings.GetNewLocalObject() : bindings.GetNewFinalizer(), false};
    }
    const auto& package = def.GetPackageName();
    const auto& name = def.GetSrcCodeIdentifier();
    if (package == REF_PACKAGE_NAME && name == STD_LIB_WEAK_REF) {
        return {local ? bindings.GetNewLocalWeakRefObject() : bindings.GetNewWeakRefObject(), false};
    }
    if ((package == CORE_PACKAGE_NAME && name == STD_LIB_FUTURE) ||
        (package == SYNC_PACKAGE_NAME &&
            (name == STD_LIB_MONITOR || name == STD_LIB_MUTEX || name == STD_LIB_WAIT_QUEUE))) {
        return {bindings.GetNewPinnedObject(), true};
    }
    return {local ? bindings.GetNewLocalObject() : bindings.GetNewObject(), false};
}

void CompileAllocation(FunctionContext& context, const CHIR::AllocateBase& expression,
    const CHIR::Block* normal, const CHIR::Block* unwind)
{
    const auto* type = expression.GetType();
    const auto* result = expression.GetResult();
    const auto* resultType = result ? result->GetType() : nullptr;
    if (!type || !type->IsClass() || !resultType || !resultType->IsRef() ||
        static_cast<const CHIR::RefType*>(resultType)->GetBaseType() != type ||
        type->IsAutoEnvBase()) {
        context.Fail(JitFailureKind::UnsupportedAllocation, expression.GetExprKind());
        return;
    }
    const auto* def = static_cast<const CHIR::ClassType*>(type)->GetClassDef();
    if (!def || !def->IsClass()) {
        context.Fail(JitFailureKind::UnsupportedAllocation, expression.GetExprKind());
        return;
    }
    if (!context.GetGlobalContext().GetRuntimeBindings().EnsurePackage(def->GetPackageName())) {
        context.Fail(JitFailureKind::UnresolvedSymbol, expression.GetExprKind());
        return;
    }
    const bool local = type->IsLocalRegion();

    const bool dynamic = type->IsGenericRelated();
    auto* typeInfo = dynamic ? nullptr : context.GetTypeInfoEmitter().GetConcreteTypeInfo(*type);
    auto& bindings = context.GetGlobalContext().GetRuntimeBindings();
    auto* sizeAddress = Resolve(context, bindings.GetTypeInstanceSize());
    auto allocation = SelectAllocationFunction(bindings, *def, local);
    auto* allocateAddress = Resolve(context, std::move(allocation.address));
    if ((!dynamic && !typeInfo) || !sizeAddress || !allocateAddress) {
        context.Fail(JitFailureKind::UnresolvedSymbol, expression.GetExprKind());
        return;
    }
    using GetSize = uint32_t (*)(const void*);
    // Runtime expects the eight-byte header plus the payload rounded to eight bytes.
    const uint64_t payload = dynamic ? 0 : reinterpret_cast<GetSize>(sizeAddress)(typeInfo);
    const bool pinned = allocation.pinned;
    const uint64_t size = ((payload + 7) & ~uint64_t{7}) + 8;
    if (size > std::numeric_limits<uint32_t>::max()) {
        context.Fail(JitFailureKind::UnsupportedAllocation, expression.GetExprKind());
        return;
    }
    std::vector<CodeAddress> methods;
    if (type->IsAutoEnv()) {
        if (dynamic) {
            context.Fail(JitFailureKind::UnsupportedAllocation, expression.GetExprKind());
            return;
        }
        const auto functions = def->GetMethods();
        if (functions.size() > payload / sizeof(uint64_t) ||
            functions.size() > (std::numeric_limits<int32_t>::max() - 8) / sizeof(uint64_t)) {
            context.Fail(JitFailureKind::UnsupportedAllocation, expression.GetExprKind());
            return;
        }
        methods.reserve(functions.size());
        for (const auto* method : functions) {
            const auto address = CompileFunctionStub::Compile(context.GetGlobalContext(), method);
            if (!address) {
                context.Fail(JitFailureKind::UnsupportedCall, expression.GetExprKind());
                return;
            }
            methods.push_back(address);
        }
    }

    auto& assembler = context.GetAssembler();
    if (dynamic) {
        if (!context.GetTypeInfoEmitter().LoadTypeInfo(*type, x86::rax)) {
            return;
        }
        MemoryUtils::ReserveStack(context, 16);
        assembler.mov(x86::qword_ptr(x86::rsp), x86::rax);
        assembler.mov(x86::rdi, x86::rax);
        assembler.long_().mov(x86::r11, asmjit::imm(reinterpret_cast<uintptr_t>(sizeAddress)));
        assembler.call(x86::r11);
        assembler.mov(x86::esi, x86::eax);
        assembler.add(x86::rsi, 7);
        assembler.and_(x86::rsi, -8);
        assembler.add(x86::rsi, 8);
        assembler.mov(x86::rdi, x86::qword_ptr(x86::rsp));
    } else {
        assembler.mov(x86::rdi, asmjit::imm(reinterpret_cast<uintptr_t>(typeInfo)));
        assembler.mov(x86::esi, static_cast<uint32_t>(size));
    }
    if (pinned) {
        assembler.xor_(x86::edx, x86::edx);
    }
    assembler.long_().mov(x86::r11, asmjit::imm(reinterpret_cast<uintptr_t>(allocateAddress)));
    const auto callStart = assembler.offset();
    assembler.call(x86::r11);
    const auto callEnd = assembler.offset();
    if (!context.GetSafepointRecorder().Record(expression)) {
        return;
    }
    if (dynamic) {
        MemoryUtils::ReleaseStack(context, 16);
    }
    if (!MemoryUtils::StoreValue(context, result, x86::rax)) {
        return;
    }
    for (size_t i = 0; i < methods.size(); ++i) {
        assembler.long_().mov(x86::r11, asmjit::imm(methods[i]));
        assembler.mov(x86::qword_ptr(x86::rax, static_cast<int32_t>(8 + i * sizeof(uint64_t))), x86::r11);
    }
    if (local && def->GetFinalizer()) {
        auto* addFinalizer = Resolve(context, bindings.GetAddLocalFinalizer());
        if (!addFinalizer || !MemoryUtils::LoadValue(context, result, x86::rdi)) {
            return;
        }
        assembler.long_().mov(x86::r11, asmjit::imm(reinterpret_cast<uintptr_t>(addFinalizer)));
        assembler.call(x86::r11);
    }
    if (normal && unwind && !context.GetFailure()) {
        assembler.jmp(context.GetLocalFinalizers().GetEdgeLabel(normal));
        context.GetExceptionLandingEmitter().Emit(*unwind, callStart, callEnd);
    }
}

void CompileRegion(FunctionContext& context, const CHIR::Expression& expression, SymbolResult symbol)
{
    auto* address = Resolve(context, std::move(symbol));
    if (!address) {
        return;
    }
    auto& assembler = context.GetAssembler();
    assembler.long_().mov(x86::r11, asmjit::imm(reinterpret_cast<uintptr_t>(address)));
    assembler.call(x86::r11);
    context.GetSafepointRecorder().Record(expression);
}
} // namespace

void CompileObjectAllocate(FunctionContext& context, const CHIR::Allocate& expression)
{
    CompileAllocation(context, expression, nullptr, nullptr);
}

void CompileObjectAllocate(FunctionContext& context, const CHIR::TryAllocate& expression)
{
    if (!expression.GetSuccessBlock() || !expression.GetErrorBlock()) {
        context.Fail(JitFailureKind::MissingBranchDestination);
        return;
    }
    CompileAllocation(context, expression, expression.GetSuccessBlock(), expression.GetErrorBlock());
}

void CompileLocalRegion(FunctionContext& context, const CHIR::StartRegion& expression)
{
    CompileRegion(context, expression, context.GetGlobalContext().GetRuntimeBindings().GetStartLocalRegion());
}

void CompileLocalRegion(FunctionContext& context, const CHIR::EndRegion& expression)
{
    CompileRegion(context, expression, context.GetGlobalContext().GetRuntimeBindings().GetEndLocalRegion());
}
} // namespace Cangjie::JIT::X86_64
