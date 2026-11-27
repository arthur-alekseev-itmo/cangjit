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
#include "../../JitAssert.h"
#include "../../Utils/MemoryUtils.h"
#include "../../Utils/RegisterUtils.h"
#include "../../../Runtime/RuntimeError.h"
#include "cangjie/CHIR/IR/Type/ClassDef.h"
#include "cangjie/CHIR/IR/Value/Value.h"
#include "cangjie/Utils/ConstantsUtils.h"

namespace Cangjie::JIT::X86_64 {
namespace {
namespace x86 = asmjit::x86;

struct AllocationFunction {
    void* address;
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
    auto type = expression.GetType();
    auto result = expression.GetResult();
    auto resultType = result ? result->GetType() : nullptr;
    // Scalar allocations go through CompileAllocate in CompileBlock.
    JIT_ASSERT(type && type->IsClass() && resultType && resultType->IsRef() &&
        static_cast<const CHIR::RefType*>(resultType)->GetBaseType() == type && !type->IsAutoEnvBase(),
        "object allocation produces a class reference");
    auto def = static_cast<const CHIR::ClassType*>(type)->GetClassDef();
    JIT_ASSERT(def && def->IsClass(), "allocated class has a class definition");
    Require(context.GetGlobalContext().GetRuntimeBindings().EnsurePackage(def->GetPackageName()));
    const bool local = type->IsLocalRegion();

    const bool dynamic = type->IsGenericRelated();
    auto typeInfo = dynamic ? nullptr : context.GetTypeInfoEmitter().GetConcreteTypeInfo(*type);
    auto& bindings = context.GetGlobalContext().GetRuntimeBindings();
    auto sizeAddress = bindings.GetTypeInstanceSize();
    auto allocation = SelectAllocationFunction(bindings, *def, local);
    auto allocateAddress = allocation.address;
    JIT_ASSERT(dynamic || typeInfo != nullptr, "allocated class has concrete type information");
    using GetSize = uint32_t (*)(const void*);
    // Runtime expects the eight-byte header plus the payload rounded to eight bytes.
    const uint64_t payload = dynamic ? 0 : reinterpret_cast<GetSize>(sizeAddress)(typeInfo);
    const bool pinned = allocation.pinned;
    const uint64_t size = ((payload + 7) & ~uint64_t{7}) + 8;
    JIT_ASSERT(size <= std::numeric_limits<uint32_t>::max(), "allocation size fits in 32 bits");
    std::vector<CodeAddress> methods;
    if (type->IsAutoEnv()) {
        JIT_ASSERT(!dynamic, "generic closure environments are not allocated eagerly");
        const auto functions = def->GetMethods();
        JIT_ASSERT(functions.size() <= payload / sizeof(uint64_t) &&
            functions.size() <= (static_cast<size_t>(std::numeric_limits<int32_t>::max()) - 8) / sizeof(uint64_t),
            "closure environment layout has room for every captured method");
        methods.reserve(functions.size());
        for (auto method : functions) {
            methods.push_back(CompileFunctionStub::Compile(context.GetGlobalContext(), method));
        }
    }

    auto& assembler = context.GetAssembler();
    if (dynamic) {
        context.GetTypeInfoEmitter().LoadTypeInfo(*type, x86::rax);
        assembler.sub(x86::rsp, 16);
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
    context.GetSafepointRecorder().Record(expression);
    if (dynamic) {
        assembler.add(x86::rsp, 16);
    }
    auto resultLocation = context.GetValueLocation(result);
    MemoryUtils::Move(context, *resultLocation, RegisterUtils::ToLocation(x86::rax),
        *result->GetType(), x86::r11);
    for (size_t i = 0; i < methods.size(); ++i) {
        assembler.long_().mov(x86::r11, asmjit::imm(methods[i]));
        assembler.mov(x86::qword_ptr(x86::rax, static_cast<int32_t>(8 + i * sizeof(uint64_t))), x86::r11);
    }
    if (local && def->GetFinalizer()) {
        auto addFinalizer = bindings.GetAddLocalFinalizer();
        MemoryUtils::Move(context, RegisterUtils::ToLocation(x86::rdi), *resultLocation,
            *result->GetType(), x86::r11);
        assembler.long_().mov(x86::r11, asmjit::imm(reinterpret_cast<uintptr_t>(addFinalizer)));
        assembler.call(x86::r11);
    }
    if (normal && unwind) {
        assembler.jmp(context.GetBlockLabel(normal));
        context.GetExceptionLandingEmitter().Emit(*unwind, callStart, callEnd);
    }
}

void CompileRegion(FunctionContext& context, const CHIR::Expression& expression, void* symbol)
{
    auto& assembler = context.GetAssembler();
    assembler.long_().mov(x86::r11, asmjit::imm(reinterpret_cast<uintptr_t>(symbol)));
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
    JIT_ASSERT(expression.GetSuccessBlock() && expression.GetErrorBlock(),
        "try allocate has success and error destinations");
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
