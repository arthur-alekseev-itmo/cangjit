// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "FunctionContext.h"

#include "GlobalContext.h"
#include "../../Utils/MemoryUtils.h"
#include "../../RegisterAllocation/SafepointLiveness.h"
#include "../../Utils/StackFrameUtils.h"

#include <algorithm>
#include <cassert>
#include <functional>
#include <limits>
#include <ostream>
#include <utility>

#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Type/ClassDef.h"
#include "cangjie/CHIR/IR/Type/ExtendDef.h"
#include "cangjie/Mangle/CHIRTypeManglingUtils.h"
#include "cangjie/CHIR/IR/Value/Value.h"

namespace Cangjie::JIT::X86_64 {
namespace {
bool FindGenericPath(const CHIR::Type& type, const CHIR::Type& generic, std::vector<size_t>& path)
{
    if (&type == &generic) {
        return true;
    }
    const CHIR::Type* base = &type;
    while (base->IsRef()) {
        base = static_cast<const CHIR::RefType*>(base)->GetBaseType();
        if (!base) {
            return false;
        }
    }
    const auto args = base->GetTypeArgs();
    for (size_t i = 0; i < args.size(); ++i) {
        if (!args[i]) {
            continue;
        }
        path.push_back(i);
        if (FindGenericPath(*args[i], generic, path)) {
            return true;
        }
        path.pop_back();
    }
    return false;
}
}

FunctionContext::FunctionContext(GlobalContext& globalContext, const CHIR::Function* function,
    ValueNumeration valueNumeration, InterferenceGraph interferenceGraph, RegisterAllocation registerAllocation) noexcept
    : globalContext(globalContext),
      error(asmjit::kErrorOk),
      metadataBuilder(globalContext.IsStackGrowthEnabled()),
      function(function),
      valueNumeration(std::move(valueNumeration)),
      interferenceGraph(std::move(interferenceGraph)),
      registerAllocation(std::move(registerAllocation))
{
    assert(function != nullptr);
    const auto genericCount = function->GetGenericTypeParams().size();
    if (genericCount > std::numeric_limits<size_t>::max() - 2) {
        Fail(JitFailureKind::StackFrameTooLarge);
        return;
    }
    hiddenTypeInfoCount = genericCount + (function->GetParentCustomTypeDef() ? 1U : 0U) +
        (function->TestAttr(CHIR::Attribute::STATIC) ? 1U : 0U);
    const auto hiddenBase = this->registerAllocation.ReserveStackSlots(hiddenTypeInfoCount);
    if (!hiddenBase) {
        Fail(JitFailureKind::StackFrameTooLarge);
        return;
    }
    hiddenTypeInfoBase = *hiddenBase;
    error = code.init(globalContext.GetRuntime().environment());
    if (error == asmjit::kErrorOk) {
        code.setErrorHandler(&emitterErrorHandler);
        error = code.attach(&assembler);
        if (error == asmjit::kErrorOk) {
            epilogueLabel = assembler.newLabel();
            if (!epilogueLabel.isValid()) {
                Fail(JitFailureKind::EpilogueLabelCreationFailed);
            }
        }
    }
}

GlobalContext& FunctionContext::GetGlobalContext() const noexcept
{
    return globalContext;
}

asmjit::CodeHolder& FunctionContext::GetCode() noexcept
{
    return code;
}

asmjit::x86::Assembler& FunctionContext::GetAssembler() noexcept
{
    return assembler;
}
JitMetadataBuilder& FunctionContext::GetMetadataBuilder() noexcept
{
    return metadataBuilder;
}

std::optional<int32_t> FunctionContext::GetHiddenTypeInfoOffset(size_t index) const noexcept
{
    if (index >= hiddenTypeInfoCount) {
        return std::nullopt;
    }
    return StackFrameUtils::GetSlotOffset(hiddenTypeInfoBase + index, registerAllocation.GetStackSlotCount());
}

bool FunctionContext::LoadTypeInfo(const CHIR::Type& input, const asmjit::x86::Gp& target)
{
    const CHIR::Type* type = &input;
    while (type->IsRef()) {
        type = static_cast<const CHIR::RefType*>(type)->GetBaseType();
        if (!type) {
            Fail(JitFailureKind::UnsupportedCall);
            return false;
        }
    }
    const auto* parent = function->GetParentCustomTypeDef();
    const auto* parentType = parent
        ? (parent->IsExtend() ? static_cast<const CHIR::ExtendDef*>(parent)->GetExtendedType() : parent->GetType())
        : nullptr;
    if (parent && !parent->GetGenericTypeParams().empty() && type == parentType) {
        const auto offset = GetHiddenTypeInfoOffset(function->GetGenericTypeParams().size());
        if (!offset) {
            Fail(JitFailureKind::UnsupportedCall);
            return false;
        }
        assembler.mov(target, asmjit::x86::qword_ptr(asmjit::x86::rbp, *offset));
        return true;
    }
    if (type->IsGeneric()) {
        const auto& generics = function->GetGenericTypeParams();
        for (size_t i = 0; i < generics.size(); ++i) {
            if (generics[i] == type) {
                const auto offset = GetHiddenTypeInfoOffset(i);
                if (!offset) {
                    break;
                }
                assembler.mov(target, asmjit::x86::qword_ptr(asmjit::x86::rbp, *offset));
                return true;
            }
        }
        if (parent) {
            const auto& outerGenerics = parent->GetGenericTypeParams();
            for (size_t i = 0; i < outerGenerics.size(); ++i) {
                if (outerGenerics[i] != type) {
                    continue;
                }
                std::vector<size_t> path;
                if (parent->IsExtend()) {
                    if (!parentType || !FindGenericPath(*parentType, *type, path) || path.empty()) {
                        break;
                    }
                } else {
                    path.push_back(i);
                }
                const auto offset = GetHiddenTypeInfoOffset(generics.size());
                if (!offset) {
                    break;
                }
                assembler.mov(target, asmjit::x86::qword_ptr(asmjit::x86::rbp, *offset));
                for (const auto index : path) {
                    if (index > static_cast<size_t>(std::numeric_limits<int32_t>::max()) / sizeof(uint64_t)) {
                        Fail(JitFailureKind::UnsupportedCall);
                        return false;
                    }
                    // x86-64 native TypeInfo.typeArgs.
                    assembler.mov(target, asmjit::x86::qword_ptr(target, 48));
                    assembler.mov(target, asmjit::x86::qword_ptr(target, static_cast<int32_t>(index * sizeof(uint64_t))));
                }
                return true;
            }
        }
        Fail(JitFailureKind::UnsupportedCall);
        return false;
    }
    if (type->IsThis()) {
        if (function->TestAttr(CHIR::Attribute::STATIC)) {
            const auto index = function->GetGenericTypeParams().size() +
                (function->GetParentCustomTypeDef() ? 1U : 0U);
            const auto offset = GetHiddenTypeInfoOffset(index);
            if (!offset) {
                Fail(JitFailureKind::UnsupportedCall);
                return false;
            }
            assembler.mov(target, asmjit::x86::qword_ptr(asmjit::x86::rbp, *offset));
            return true;
        }
        const auto& parameters = function->GetParams();
        if (parameters.empty() || !MemoryUtils::LoadValue(*this, parameters.front(), target)) {
            Fail(JitFailureKind::UnsupportedCall);
            return false;
        }
        assembler.mov(target, asmjit::x86::qword_ptr(target));
        assembler.shl(target, 16);
        assembler.shr(target, 16);
        return true;
    }
    if (type->IsGenericRelated()) {
        auto arguments = type->GetTypeArgs();
        std::string templateName;
        if (type->IsClass() || type->IsStruct() || type->IsEnum()) {
            const auto* custom = &static_cast<const CHIR::CustomType&>(*type);
            if (custom->IsAutoEnvBase()) {
                auto& builder = globalContext.GetCHIRBuilder();
                if (custom->IsAutoEnvInstBase()) {
                    custom = const_cast<CHIR::ClassType*>(static_cast<const CHIR::ClassType*>(custom))
                                 ->GetSuperClassTy(&builder);
                }
                if (!custom || !custom->IsAutoEnvGenericBase()) {
                    Fail(JitFailureKind::UnsupportedCall);
                    return false;
                }
                auto args = custom->GetGenericArgs();
                if (args.empty()) {
                    Fail(JitFailureKind::UnsupportedCall);
                    return false;
                }
                std::vector<CHIR::Type*> parameters(args.begin(), args.end() - 1);
                arguments = {builder.GetType<CHIR::FuncType>(parameters, args.back())};
                templateName = "Closure.tt";
            } else {
                const auto* def = custom->GetCustomTypeDef();
                if (!def || !globalContext.GetRuntimeBindings().EnsureStandardPackage(def->GetPackageName()).empty()) {
                    Fail(JitFailureKind::UnresolvedSymbol);
                    return false;
                }
                const auto& package =
                    def->GetGenericDecl() ? def->GetGenericDecl()->GetPackageName() : def->GetPackageName();
                templateName = package + ":" + CHIR::GetCustomTypeIdentifier(*custom) + ".tt";
            }
        } else if (type->IsTuple()) {
            templateName = "Tuple.tt";
        } else if (type->IsRawArray()) {
            templateName = "RawArray.tt";
        } else if (type->IsCPointer()) {
            templateName = "CPointer.tt";
        } else if (type->IsBox()) {
            templateName = "Box.tt";
        } else if (type->IsFunc()) {
            templateName = "CFunc.tt";
            if (!arguments.empty()) {
                std::rotate(arguments.begin(), arguments.end() - 1, arguments.end());
            }
        } else {
            Fail(JitFailureKind::UnsupportedCall);
            return false;
        }
        if (arguments.empty() || arguments.size() > (static_cast<size_t>(std::numeric_limits<int32_t>::max()) - 15) /
                sizeof(uint64_t)) {
            Fail(JitFailureKind::UnsupportedCall);
            return false;
        }
        auto templateSymbol = globalContext.GetRuntimeBindings().Resolve(templateName);
        auto helperSymbol = globalContext.GetRuntimeBindings().Resolve("CJ_MCC_GetOrCreateTypeInfo");
        const auto* typeTemplate = std::get_if<void*>(&templateSymbol);
        const auto* helper = std::get_if<void*>(&helperSymbol);
        if (!typeTemplate || !*typeTemplate || !helper || !*helper) {
            Fail(JitFailureKind::UnresolvedSymbol);
            return false;
        }
        const auto stackSize = static_cast<uint32_t>((arguments.size() * sizeof(uint64_t) + 15) & ~size_t{15});
        MemoryUtils::ReserveStack(*this, stackSize);
        for (size_t i = 0; i < arguments.size(); ++i) {
            if (!arguments[i] || !LoadTypeInfo(*arguments[i], asmjit::x86::rax)) {
                return false;
            }
            assembler.mov(asmjit::x86::qword_ptr(asmjit::x86::rsp, static_cast<int32_t>(i * sizeof(uint64_t))),
                asmjit::x86::rax);
        }
        assembler.long_().mov(asmjit::x86::rdi, asmjit::imm(reinterpret_cast<uintptr_t>(*typeTemplate)));
        assembler.mov(asmjit::x86::esi, static_cast<uint32_t>(arguments.size()));
        assembler.mov(asmjit::x86::rdx, asmjit::x86::rsp);
        assembler.long_().mov(asmjit::x86::r11, asmjit::imm(reinterpret_cast<uintptr_t>(*helper)));
        // Native CodeGen emits llvm.cj.get.type.info as a nounwind, readonly
        // intrinsic; its runtime helper is not a managed safepoint.
        assembler.call(asmjit::x86::r11);
        MemoryUtils::ReleaseStack(*this, stackSize);
        if (target != asmjit::x86::rax) {
            assembler.mov(target, asmjit::x86::rax);
        }
        return true;
    }
    if (type->IsClass() || type->IsStruct() || type->IsEnum()) {
        const auto* def = static_cast<const CHIR::CustomType*>(type)->GetCustomTypeDef();
        if (!def || !globalContext.GetRuntimeBindings().EnsureStandardPackage(def->GetPackageName()).empty()) {
            Fail(JitFailureKind::UnresolvedSymbol);
            return false;
        }
    }
    auto symbol = globalContext.GetRuntimeBindings().Resolve("CJ_MCC_GetTypeByMangledName");
    const auto* lookup = std::get_if<void*>(&symbol);
    if (!lookup || !*lookup) {
        Fail(JitFailureKind::UnresolvedSymbol);
        return false;
    }
    const auto name = CHIR::GetTypeQualifiedName(*type, true);
    auto* info = reinterpret_cast<void* (*)(const char*)>(*lookup)(name.c_str());
    if (!info) {
        Fail(JitFailureKind::UnresolvedSymbol);
        return false;
    }
    assembler.long_().mov(target, asmjit::imm(reinterpret_cast<uintptr_t>(info)));
    return true;
}
bool FunctionContext::RecordSafepoint(const CHIR::Expression& expression, const CHIR::Value* temporaryRoot)
{
    const auto offset = assembler.offset();
    if (offset < 4 || offset - 4 > std::numeric_limits<uint32_t>::max()) {
        Fail(JitFailureKind::UnsupportedExpression, expression.GetExprKind());
        return false;
    }
    auto result = ComputeSafepointLiveness(*function, valueNumeration, registerAllocation, expression);
    auto* roots = std::get_if<SafepointRoots>(&result);
    if (!roots) {
        Fail(JitFailureKind::UnsupportedExpression, expression.GetExprKind());
        return false;
    }
    auto& [gc, stack, derived] = *roots;
    if (temporaryRoot) {
        const auto id = valueNumeration.GetId(temporaryRoot);
        const auto* slot = id && *id < registerAllocation.GetLocationCount()
            ? registerAllocation.GetLocation(*id).AsStack() : nullptr;
        const auto rootOffset = slot
            ? StackFrameUtils::GetSlotOffset(slot->index, registerAllocation.GetStackSlotCount()) : std::nullopt;
        if (!slot || !rootOffset || gc.first != 0) {
            Fail(JitFailureKind::UnsupportedExpression, expression.GetExprKind());
            return false;
        }
        for (ValueNumeration::Id i = 0; i < registerAllocation.GetLocationCount(); ++i) {
            const auto* other = registerAllocation.GetLocation(i).AsStack();
            if ((i != *id && other && other->index == slot->index) ||
                registerAllocation.GetAllocationSlot(i) == slot->index) {
                Fail(JitFailureKind::UnsupportedExpression, expression.GetExprKind());
                return false;
            }
        }
        auto position = std::lower_bound(gc.second.begin(), gc.second.end(), *rootOffset, std::greater<int32_t>());
        const auto index = position - gc.second.begin();
        gc.second.insert(position, *rootOffset);
        derived.insert(derived.begin() + index, JitMetadataBuilder::Roots{});
    }
    if (!globalContext.IsStackGrowthEnabled()) {
        stack = {};
    }
    metadataBuilder.RecordSafepoint(static_cast<uint32_t>(offset - 4), gc.first, std::move(gc.second),
        stack.first, std::move(stack.second), 0, std::move(derived));
    return true;
}
bool FunctionContext::EmitExceptionLanding(const CHIR::Block& block, size_t callStart, size_t callEnd)
{
    const auto landingOffset = assembler.offset();
    if (!block.IsLandingPadBlock() || callStart < 4 || callEnd <= callStart ||
        callEnd - 4 > std::numeric_limits<uint32_t>::max() ||
        landingOffset < 4 || landingOffset - 4 > std::numeric_limits<uint32_t>::max()) {
        Fail(JitFailureKind::UnsupportedExpression);
        return false;
    }
    std::vector<uintptr_t> types;
    const auto exceptions = block.GetExceptions();
    if (exceptions.empty()) {
        types.push_back(0);
    } else {
        auto symbol = globalContext.GetRuntimeBindings().Resolve("CJ_MCC_GetTypeByMangledName");
        const auto* address = std::get_if<void*>(&symbol);
        if (!address || !*address) {
            Fail(JitFailureKind::UnresolvedSymbol);
            return false;
        }
        using LookupType = void* (*)(const char*);
        const auto lookup = reinterpret_cast<LookupType>(*address);
        types.reserve(exceptions.size());
        for (const auto* type : exceptions) {
            if (!type) {
                Fail(JitFailureKind::UnsupportedExpression);
                return false;
            }
            const auto* def = type->GetClassDef();
            if (!def || !globalContext.GetRuntimeBindings().EnsureStandardPackage(def->GetPackageName()).empty()) {
                Fail(JitFailureKind::UnresolvedSymbol);
                return false;
            }
            const auto name = CHIR::GetTypeQualifiedName(*type, true);
            auto* info = lookup(name.c_str());
            if (!info) {
                Fail(JitFailureKind::UnresolvedSymbol);
                return false;
            }
            types.push_back(reinterpret_cast<uintptr_t>(info));
        }
    }
    const auto slots = registerAllocation.GetStackSlotCount();
    if (slots == std::numeric_limits<size_t>::max()) {
        Fail(JitFailureKind::StackFrameTooLarge);
        return false;
    }
    const auto frameSize = StackFrameUtils::GetFrameSize(slots + 1);
    if (!frameSize) {
        Fail(JitFailureKind::StackFrameTooLarge);
        return false;
    }
    const auto landing = metadataBuilder.RecordLandingPad(static_cast<uint32_t>(landingOffset - 4), std::move(types));
    metadataBuilder.RecordCallSite(static_cast<uint32_t>(callStart - 4),
        static_cast<uint32_t>(callEnd - callStart), landing);
    assembler.lea(asmjit::x86::rsp, asmjit::x86::ptr(asmjit::x86::rbp, -static_cast<int32_t>(*frameSize)));
    assembler.jmp(GetBlockLabel(&block));
    return GetError() == asmjit::kErrorOk;
}




asmjit::Error FunctionContext::GetError() const noexcept
{
    return error == asmjit::kErrorOk ? emitterErrorHandler.firstError : error;
}


asmjit::Label FunctionContext::GetBlockLabel(const CHIR::Block* block)
{
    if (!block) {
        Fail(JitFailureKind::MissingBranchDestination);
        return {};
    }
    auto found = blockLabels.find(block);
    if (found != blockLabels.end()) {
        return found->second;
    }
    auto label = assembler.newLabel();
    if (!label.isValid()) {
        Fail(JitFailureKind::BlockLabelCreationFailed);
        return label;
    }
    blockLabels.emplace(block, label);
    return label;
}

asmjit::Label FunctionContext::GetEpilogueLabel() const noexcept
{
    return epilogueLabel;
}

void FunctionContext::Fail(JitFailureKind kind, std::optional<CHIR::ExprKind> expressionKind) noexcept
{
    if (!failure) {
        failure = JitFailure{kind, expressionKind};
    }
}

const std::optional<JitFailure>& FunctionContext::GetFailure() const noexcept
{
    return failure;
}

const CHIR::Function* FunctionContext::GetFunction() const noexcept
{
    return function;
}

std::vector<const CHIR::Function*>& FunctionContext::GetFunctionsToStub() noexcept
{
    return functionsToStub;
}

const std::vector<const CHIR::Function*>& FunctionContext::GetFunctionsToStub() const noexcept
{
    return functionsToStub;
}
std::vector<PendingLazyCallSite>& FunctionContext::GetLazyCallSites() noexcept
{
    return lazyCallSites;
}


const ValueNumeration& FunctionContext::GetValueNumeration() const noexcept
{
    return valueNumeration;
}

const InterferenceGraph& FunctionContext::GetInterferenceGraph() const noexcept
{
    return interferenceGraph;
}

const RegisterAllocation& FunctionContext::GetRegisterAllocation() const noexcept
{
    return registerAllocation;
}

void FunctionContext::Dump(std::ostream& output) const
{
    output << "FunctionContext(function=" << function->GetIdentifier() << ")\n";
    output << "Body:\n" << function->GetBody()->ToString(0) << '\n';
    valueNumeration.Dump(output);
    interferenceGraph.Dump(output);
    registerAllocation.Dump(output);
}

} // namespace Cangjie::JIT::X86_64
