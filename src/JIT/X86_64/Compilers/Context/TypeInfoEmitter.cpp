// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "TypeInfoEmitter.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "FunctionContext.h"
#include "GlobalContext.h"
#include "../../Utils/MemoryUtils.h"
#include "../../Utils/RegisterUtils.h"
#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Type/ClassDef.h"
#include "cangjie/CHIR/IR/Type/ExtendDef.h"
#include "cangjie/CHIR/IR/Value/Value.h"
#include "cangjie/Mangle/CHIRTypeManglingUtils.h"

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

struct TypeTemplateInfo {
    void* typeTemplate;
    void* create;
    std::vector<CHIR::Type*> arguments;
};

std::optional<std::string> PrepareClosureTemplate(
    FunctionContext& context, const CHIR::CustomType& type, std::vector<CHIR::Type*>& arguments)
{
    auto custom = &type;
    auto& builder = context.GetGlobalContext().GetCHIRBuilder();
    if (custom->IsAutoEnvInstBase()) {
        custom = const_cast<CHIR::ClassType*>(static_cast<const CHIR::ClassType*>(custom))
                     ->GetSuperClassTy(&builder);
    }
    if (!custom || !custom->IsAutoEnvGenericBase()) {
        context.Fail(JitFailureKind::UnsupportedCall);
        return std::nullopt;
    }
    auto args = custom->GetGenericArgs();
    if (args.empty()) {
        context.Fail(JitFailureKind::UnsupportedCall);
        return std::nullopt;
    }
    std::vector<CHIR::Type*> parameters(args.begin(), args.end() - 1);
    arguments = {builder.GetType<CHIR::FuncType>(parameters, args.back())};
    return "Closure.tt";
}

std::optional<std::string> GetCustomTemplateName(
    FunctionContext& context, const CHIR::CustomType& custom, std::vector<CHIR::Type*>& arguments)
{
    if (custom.IsAutoEnvBase()) {
        return PrepareClosureTemplate(context, custom, arguments);
    }
    auto def = custom.GetCustomTypeDef();
    if (!def || !context.GetGlobalContext().GetRuntimeBindings().EnsurePackage(def->GetPackageName())) {
        context.Fail(JitFailureKind::UnresolvedSymbol);
        return std::nullopt;
    }
    const auto& package = def->GetGenericDecl() ? def->GetGenericDecl()->GetPackageName() : def->GetPackageName();
    return package + ":" + CHIR::GetCustomTypeIdentifier(custom) + ".tt";
}

std::optional<std::string> GetTypeTemplateName(
    FunctionContext& context, const CHIR::Type& type, std::vector<CHIR::Type*>& arguments)
{
    if (type.IsClass() || type.IsStruct() || type.IsEnum()) {
        return GetCustomTemplateName(context, static_cast<const CHIR::CustomType&>(type), arguments);
    }
    if (type.IsTuple()) {
        return "Tuple.tt";
    }
    if (type.IsRawArray()) {
        return "RawArray.tt";
    }
    if (type.IsCPointer()) {
        return "CPointer.tt";
    }
    if (type.IsBox()) {
        return "Box.tt";
    }
    if (type.IsFunc()) {
        if (!arguments.empty()) {
            std::rotate(arguments.begin(), arguments.end() - 1, arguments.end());
        }
        return "CFunc.tt";
    }
    context.Fail(JitFailureKind::UnsupportedCall);
    return std::nullopt;
}

std::optional<TypeTemplateInfo> PrepareTypeTemplate(FunctionContext& context, const CHIR::Type& type)
{
    auto arguments = type.GetTypeArgs();
    auto name = GetTypeTemplateName(context, type, arguments);
    if (!name) {
        return std::nullopt;
    }
    if (arguments.empty() || arguments.size() > (static_cast<size_t>(std::numeric_limits<int32_t>::max()) - 15) /
            sizeof(uint64_t)) {
        context.Fail(JitFailureKind::UnsupportedCall);
        return std::nullopt;
    }
    auto& bindings = context.GetGlobalContext().GetRuntimeBindings();
    auto templateSymbol = bindings.Resolve(*name);
    auto helperSymbol = bindings.GetCreateTypeInfo();
    if (!templateSymbol || !helperSymbol) {
        context.Fail(JitFailureKind::UnresolvedSymbol);
        return std::nullopt;
    }
    return TypeTemplateInfo{templateSymbol.Value(), helperSymbol.Value(), std::move(arguments)};
}

void* ResolveConcreteTypeInfo(FunctionContext& context, const CHIR::Type& input)
{
    const CHIR::Type* type = &input;
    while (type->IsRef()) {
        type = static_cast<const CHIR::RefType*>(type)->GetBaseType();
        if (!type) {
            context.Fail(JitFailureKind::UnsupportedCall);
            return nullptr;
        }
    }
    const bool isCustom = type->IsClass() || type->IsStruct() || type->IsEnum();
    auto custom = isCustom ? static_cast<const CHIR::CustomType*>(type) : nullptr;
    if (custom && !custom->IsAutoEnvBase()) {
        auto def = custom->GetCustomTypeDef();
        if (!def || !context.GetGlobalContext().GetRuntimeBindings().EnsurePackage(def->GetPackageName())) {
            context.Fail(JitFailureKind::UnresolvedSymbol);
            return nullptr;
        }
    }
    if (!custom || !custom->IsAutoEnvBase()) {
        auto symbol = context.GetGlobalContext().GetRuntimeBindings().GetTypeByName();
        if (!symbol) {
            context.Fail(JitFailureKind::UnresolvedSymbol);
            return nullptr;
        }
        const auto name = CHIR::GetTypeQualifiedName(*type, true);
        if (auto found = reinterpret_cast<void* (*)(const char*)>(symbol.Value())(name.c_str())) {
            return found;
        }
    }
    auto prepared = PrepareTypeTemplate(context, *type);
    if (!prepared) {
        return nullptr;
    }
    std::vector<void*> infos;
    infos.reserve(prepared->arguments.size());
    for (auto arg : prepared->arguments) {
        auto info = arg ? ResolveConcreteTypeInfo(context, *arg) : nullptr;
        if (!info) {
            context.Fail(JitFailureKind::UnresolvedSymbol);
            return nullptr;
        }
        infos.push_back(info);
    }
    return reinterpret_cast<void* (*)(void*, uint32_t, void**)>(prepared->create)(
        prepared->typeTemplate, static_cast<uint32_t>(infos.size()), infos.data());
}
}
void* TypeInfoEmitter::GetConcreteTypeInfo(const CHIR::Type& type)
{
    return ResolveConcreteTypeInfo(context, type);
}


bool TypeInfoEmitter::StoreTypeInfos(const std::vector<const CHIR::Type*>& types, int32_t rspOffset)
{
    auto& assembler = context.GetAssembler();
    for (size_t i = 0; i < types.size(); ++i) {
        if (!types[i] || !LoadTypeInfo(*types[i], asmjit::x86::rax)) {
            context.Fail(JitFailureKind::UnsupportedCall);
            return false;
        }
        const auto offset = rspOffset + static_cast<int32_t>(i * sizeof(uint64_t));
        assembler.mov(asmjit::x86::qword_ptr(asmjit::x86::rsp, offset), asmjit::x86::rax);
    }
    return context.GetError() == asmjit::kErrorOk;
}

bool TypeInfoEmitter::LoadTypeInfo(const CHIR::Type& input, const asmjit::x86::Gp& target)
{
    auto function = context.GetFunction();
    auto& assembler = context.GetAssembler();
    const CHIR::Type* type = &input;
    while (type->IsRef()) {
        type = static_cast<const CHIR::RefType*>(type)->GetBaseType();
        if (!type) {
            context.Fail(JitFailureKind::UnsupportedCall);
            return false;
        }
    }
    auto parent = function->GetParentCustomTypeDef();
    auto parentType = parent
        ? (parent->IsExtend() ? static_cast<const CHIR::ExtendDef*>(parent)->GetExtendedType() : parent->GetType())
        : nullptr;
    if (parent && !parent->GetGenericTypeParams().empty() && type == parentType) {
        const auto offset = context.GetHiddenTypeInfoOffset(function->GetGenericTypeParams().size());
        if (!offset) {
            context.Fail(JitFailureKind::UnsupportedCall);
            return false;
        }
        assembler.mov(target, asmjit::x86::qword_ptr(asmjit::x86::rbp, *offset));
        return true;
    }
    if (type->IsGeneric()) {
        const auto& generics = function->GetGenericTypeParams();
        for (size_t i = 0; i < generics.size(); ++i) {
            if (generics[i] == type) {
                const auto offset = context.GetHiddenTypeInfoOffset(i);
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
                const auto offset = context.GetHiddenTypeInfoOffset(generics.size());
                if (!offset) {
                    break;
                }
                assembler.mov(target, asmjit::x86::qword_ptr(asmjit::x86::rbp, *offset));
                for (const auto index : path) {
                    if (index > static_cast<size_t>(std::numeric_limits<int32_t>::max()) / sizeof(uint64_t)) {
                        context.Fail(JitFailureKind::UnsupportedCall);
                        return false;
                    }
                    // x86-64 native TypeInfo.typeArgs.
                    assembler.mov(target, asmjit::x86::qword_ptr(target, 48));
                    assembler.mov(target, asmjit::x86::qword_ptr(target, static_cast<int32_t>(index * sizeof(uint64_t))));
                }
                return true;
            }
        }
        context.Fail(JitFailureKind::UnsupportedCall);
        return false;
    }
    if (type->IsThis()) {
        if (function->TestAttr(CHIR::Attribute::STATIC)) {
            const auto index = function->GetGenericTypeParams().size() +
                (function->GetParentCustomTypeDef() ? 1U : 0U);
            const auto offset = context.GetHiddenTypeInfoOffset(index);
            if (!offset) {
                context.Fail(JitFailureKind::UnsupportedCall);
                return false;
            }
            assembler.mov(target, asmjit::x86::qword_ptr(asmjit::x86::rbp, *offset));
            return true;
        }
        const auto& parameters = function->GetParams();
        auto location = context.GetValueLocation(parameters.front());
        if (!MemoryUtils::Move(context, RegisterUtils::ToLocation(target), *location,
            *parameters.front()->GetType(), asmjit::x86::r11)) {
            context.Fail(JitFailureKind::UnsupportedCall);
            return false;
        }
        assembler.mov(target, asmjit::x86::qword_ptr(target));
        assembler.shl(target, 16);
        assembler.shr(target, 16);
        return true;
    }
    if (type->IsGenericRelated()) {
        auto prepared = PrepareTypeTemplate(context, *type);
        if (!prepared) {
            return false;
        }
        const auto& arguments = prepared->arguments;
        const auto stackSize = static_cast<uint32_t>((arguments.size() * sizeof(uint64_t) + 15) & ~size_t{15});
        assembler.sub(asmjit::x86::rsp, stackSize);
        for (size_t i = 0; i < arguments.size(); ++i) {
            if (!arguments[i] || !LoadTypeInfo(*arguments[i], asmjit::x86::rax)) {
                return false;
            }
            assembler.mov(asmjit::x86::qword_ptr(asmjit::x86::rsp, static_cast<int32_t>(i * sizeof(uint64_t))),
                asmjit::x86::rax);
        }
        assembler.long_().mov(asmjit::x86::rdi, asmjit::imm(reinterpret_cast<uintptr_t>(prepared->typeTemplate)));
        assembler.mov(asmjit::x86::esi, static_cast<uint32_t>(arguments.size()));
        assembler.mov(asmjit::x86::rdx, asmjit::x86::rsp);
        assembler.long_().mov(asmjit::x86::r11, asmjit::imm(reinterpret_cast<uintptr_t>(prepared->create)));
        // Native CodeGen emits llvm.cj.get.type.info as a nounwind, readonly
        // intrinsic; its runtime helper is not a managed safepoint.
        assembler.call(asmjit::x86::r11);
        assembler.add(asmjit::x86::rsp, stackSize);
        if (target != asmjit::x86::rax) {
            assembler.mov(target, asmjit::x86::rax);
        }
        return true;
    }
    auto info = GetConcreteTypeInfo(*type);
    if (!info) {
        context.Fail(JitFailureKind::UnresolvedSymbol);
        return false;
    }
    assembler.long_().mov(target, asmjit::imm(reinterpret_cast<uintptr_t>(info)));
    return true;
}
} // namespace Cangjie::JIT::X86_64
