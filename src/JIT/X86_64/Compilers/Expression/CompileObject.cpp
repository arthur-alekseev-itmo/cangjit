// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "CompileObject.h"

#include <cstdint>
#include <limits>
#include <string_view>
#include <variant>
#include <vector>

#include "../Context/GlobalContext.h"
#include "../Function/CompileFunctionStub.h"
#include "../../Utils/MemoryUtils.h"
#include "cangjie/CHIR/IR/Type/ClassDef.h"
#include "cangjie/CHIR/IR/Value/Value.h"
#include "cangjie/Mangle/CHIRTypeManglingUtils.h"
#include "cangjie/Utils/ConstantsUtils.h"

namespace Cangjie::JIT::X86_64 {
namespace {
namespace x86 = asmjit::x86;

void* Resolve(FunctionContext& context, const char* name)
{
    auto result = context.GetGlobalContext().GetRuntimeBindings().Resolve(name);
    auto* address = std::get_if<void*>(&result);
    if (!address || !*address) {
        context.Fail(JitFailureKind::UnresolvedSymbol);
        return nullptr;
    }
    return *address;
}

const char* AllocationSymbol(const CHIR::ClassDef& def, bool local)
{
    if (def.GetFinalizer()) {
        return "CJ_MCC_NewFinalizer";
    }
    const auto& package = def.GetPackageName();
    const auto& name = def.GetSrcCodeIdentifier();
    if (package == REF_PACKAGE_NAME && name == STD_LIB_WEAK_REF) {
        return local ? "CJ_MCC_NewLocalWeakRefObject" : "CJ_MCC_NewWeakRefObject";
    }
    if ((package == CORE_PACKAGE_NAME && name == STD_LIB_FUTURE) ||
        (package == SYNC_PACKAGE_NAME &&
            (name == STD_LIB_MONITOR || name == STD_LIB_MUTEX || name == STD_LIB_WAIT_QUEUE))) {
        return "CJ_MCC_NewPinnedObject";
    }
    return local ? "CJ_MCC_NewLocalObject" : "CJ_MCC_NewObject";
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
    const bool local = type->IsLocalRegion();
    if (local && def->GetFinalizer()) {
        // A local finalizer must be removed on every dead-out frontier,
        // including exceptional edges and aliases, before its region ends.
        context.Fail(JitFailureKind::UnsupportedAllocation, expression.GetExprKind());
        return;
    }

    auto* lookupAddress = Resolve(context, "CJ_MCC_GetTypeByMangledName");
    auto* sizeAddress = Resolve(context, "_ZNK12MapleRuntime8TypeInfo15GetInstanceSizeEv");
    const std::string_view allocationSymbol = AllocationSymbol(*def, local);
    auto* allocateAddress = Resolve(context, allocationSymbol.data());
    if (!lookupAddress || !sizeAddress || !allocateAddress) {
        return;
    }
    using Lookup = void* (*)(const char*);
    using GetSize = uint32_t (*)(const void*);
    const auto name = CHIR::GetTypeQualifiedName(*type, true);
    auto* typeInfo = reinterpret_cast<Lookup>(lookupAddress)(name.c_str());
    if (!typeInfo) {
        context.Fail(JitFailureKind::UnresolvedSymbol, expression.GetExprKind());
        return;
    }
    // Runtime expects the eight-byte header plus the payload rounded to eight bytes.
    const uint64_t payload = reinterpret_cast<GetSize>(sizeAddress)(typeInfo);
    const bool pinned = allocationSymbol == "CJ_MCC_NewPinnedObject";
    const uint64_t size = pinned ? 168 : ((payload + 7) & ~uint64_t{7}) + 8;
    if (size > std::numeric_limits<uint32_t>::max()) {
        context.Fail(JitFailureKind::UnsupportedAllocation, expression.GetExprKind());
        return;
    }
    std::vector<CodeAddress> methods;
    if (type->IsAutoEnv()) {
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
    assembler.mov(x86::rdi, asmjit::imm(reinterpret_cast<uintptr_t>(typeInfo)));
    assembler.mov(x86::esi, static_cast<uint32_t>(size));
    if (pinned) {
        assembler.xor_(x86::edx, x86::edx);
    }
    assembler.long_().mov(x86::r11, asmjit::imm(reinterpret_cast<uintptr_t>(allocateAddress)));
    const auto callStart = assembler.offset();
    assembler.call(x86::r11);
    const auto callEnd = assembler.offset();
    if (!context.RecordSafepoint(expression) || !MemoryUtils::StoreValue(context, result, x86::rax)) {
        return;
    }
    for (size_t i = 0; i < methods.size(); ++i) {
        assembler.long_().mov(x86::r11, asmjit::imm(methods[i]));
        assembler.mov(x86::qword_ptr(x86::rax, static_cast<int32_t>(8 + i * sizeof(uint64_t))), x86::r11);
    }
    if (normal && unwind && !context.GetFailure()) {
        assembler.jmp(context.GetBlockLabel(normal));
        context.EmitExceptionLanding(*unwind, callStart, callEnd);
    }
}

void CompileRegion(FunctionContext& context, const CHIR::Expression& expression, const char* name)
{
    auto* address = Resolve(context, name);
    if (!address) {
        return;
    }
    auto& assembler = context.GetAssembler();
    assembler.long_().mov(x86::r11, asmjit::imm(reinterpret_cast<uintptr_t>(address)));
    assembler.call(x86::r11);
    context.RecordSafepoint(expression);
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
    CompileRegion(context, expression, "CJ_MCC_StartLocalRegion");
}

void CompileLocalRegion(FunctionContext& context, const CHIR::EndRegion& expression)
{
    CompileRegion(context, expression, "CJ_MCC_EndLocalRegion");
}
} // namespace Cangjie::JIT::X86_64
