// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "TypeInfoEmitter.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "FunctionContext.h"
#include "GlobalContext.h"
#include "../../JitAssert.h"
#include "../../../Runtime/RuntimeError.h"
#include "../../Utils/MemoryUtils.h"
#include "../../Utils/RegisterUtils.h"
#include "cangjie/CHIR/IR/Expression/Expression.h"
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

// Throws on imported package and template lookups: the loaded library set is
// environment state, not a JIT invariant.
TypeTemplateInfo PrepareTypeTemplate(FunctionContext& context, const CHIR::Type& type)
{
    auto arguments = type.GetTypeArgs();
    std::string name;
    if (type.IsClass() || type.IsStruct() || type.IsEnum()) {
        auto& custom = static_cast<const CHIR::CustomType&>(type);
        if (custom.IsAutoEnvBase()) {
            auto customBase = &custom;
            auto& builder = context.GetGlobalContext().GetCHIRBuilder();
            if (customBase->IsAutoEnvInstBase()) {
                customBase = const_cast<CHIR::ClassType*>(static_cast<const CHIR::ClassType*>(customBase))
                                 ->GetSuperClassTy(&builder);
            }
            JIT_ASSERT(customBase != nullptr && customBase->IsAutoEnvGenericBase(),
                "closure environment type has a generic base");
            auto args = customBase->GetGenericArgs();
            JIT_ASSERT(!args.empty(), "closure environment type has generic arguments");
            std::vector<CHIR::Type*> parameters(args.begin(), args.end() - 1);
            arguments = {builder.GetType<CHIR::FuncType>(parameters, args.back())};
            name = "Closure.tt";
        } else {
            auto def = custom.GetCustomTypeDef();
            JIT_ASSERT(def != nullptr, "custom type has a definition");
            Require(context.GetGlobalContext().GetRuntimeBindings().EnsurePackage(def->GetPackageName()));
            const auto& package = def->GetGenericDecl() ? def->GetGenericDecl()->GetPackageName() : def->GetPackageName();
            name = package + ":" + CHIR::GetCustomTypeIdentifier(custom) + ".tt";
        }
    } else if (type.IsTuple()) {
        name = "Tuple.tt";
    } else if (type.IsRawArray()) {
        name = "RawArray.tt";
    } else if (type.IsCPointer()) {
        name = "CPointer.tt";
    } else if (type.IsBox()) {
        name = "Box.tt";
    } else if (type.IsFunc()) {
        if (!arguments.empty()) {
            std::rotate(arguments.begin(), arguments.end() - 1, arguments.end());
        }
        name = "CFunc.tt";
    } else {
        JIT_FAIL("type template is not supported for this type");
    }
    JIT_ASSERT(!arguments.empty() && arguments.size() <=
        (static_cast<size_t>(std::numeric_limits<int32_t>::max()) - 15) / sizeof(uint64_t),
        "type template arguments are out of range");
    auto& bindings = context.GetGlobalContext().GetRuntimeBindings();
    auto typeTemplate = Require(bindings.Resolve(name));
    auto create = bindings.GetCreateTypeInfo();
    return {typeTemplate, create, std::move(arguments)};
}

void* ResolveConcreteTypeInfo(FunctionContext& context, const CHIR::Type& input)
{
    const CHIR::Type* type = &input;
    while (type->IsRef()) {
        type = static_cast<const CHIR::RefType*>(type)->GetBaseType();
        JIT_ASSERT(type != nullptr, "reference type has a base type");
    }
    const bool isCustom = type->IsClass() || type->IsStruct() || type->IsEnum();
    auto custom = isCustom ? static_cast<const CHIR::CustomType*>(type) : nullptr;
    if (custom && !custom->IsAutoEnvBase()) {
        auto def = custom->GetCustomTypeDef();
        JIT_ASSERT(def != nullptr, "custom type has a definition");
        Require(context.GetGlobalContext().GetRuntimeBindings().EnsurePackage(def->GetPackageName()));
    }
    if (!custom || !custom->IsAutoEnvBase()) {
        auto& bindings = context.GetGlobalContext().GetRuntimeBindings();
        const auto name = CHIR::GetTypeQualifiedName(*type, true);
        if (auto found = reinterpret_cast<void* (*)(const char*)>(bindings.GetTypeByName())(name.c_str())) {
            return found;
        }
    }
    auto prepared = PrepareTypeTemplate(context, *type);
    std::vector<void*> infos;
    infos.reserve(prepared.arguments.size());
    for (auto arg : prepared.arguments) {
        // Template arguments of JIT-defined types resolve recursively; a null
        // argument means the CHIR type graph is malformed.
        JIT_ASSERT(arg != nullptr, "type template argument is present");
        infos.push_back(ResolveConcreteTypeInfo(context, *arg));
    }
    return reinterpret_cast<void* (*)(void*, uint32_t, void**)>(prepared.create)(
        prepared.typeTemplate, static_cast<uint32_t>(infos.size()), infos.data());
}
}

void* TypeInfoEmitter::GetConcreteTypeInfo(const CHIR::Type& type)
{
    return ResolveConcreteTypeInfo(context, type);
}


void TypeInfoEmitter::StoreTypeInfos(const std::vector<const CHIR::Type*>& types, int32_t rspOffset)
{
    auto& assembler = context.GetAssembler();
    for (size_t i = 0; i < types.size(); ++i) {
        JIT_ASSERT(types[i] != nullptr, "staged type is present");
        LoadTypeInfo(*types[i], asmjit::x86::rax);
        const auto offset = rspOffset + static_cast<int32_t>(i * sizeof(uint64_t));
        assembler.mov(asmjit::x86::qword_ptr(asmjit::x86::rsp, offset), asmjit::x86::rax);
    }
}

void TypeInfoEmitter::LoadTypeInfo(const CHIR::Type& input, const asmjit::x86::Gp& target)
{
    auto function = context.GetFunction();
    auto& assembler = context.GetAssembler();
    const CHIR::Type* type = &input;
    while (type->IsRef()) {
        type = static_cast<const CHIR::RefType*>(type)->GetBaseType();
        JIT_ASSERT(type != nullptr, "reference type has a base type");
    }
    auto parent = function->GetParentCustomTypeDef();
    auto parentType = parent
        ? (parent->IsExtend() ? static_cast<const CHIR::ExtendDef*>(parent)->GetExtendedType() : parent->GetType())
        : nullptr;
    if (parent && !parent->GetGenericTypeParams().empty() && type == parentType) {
        assembler.mov(target,
            asmjit::x86::qword_ptr(asmjit::x86::rbp,
                context.GetHiddenTypeInfoOffset(function->GetGenericTypeParams().size())));
        return;
    }
    if (type->IsGeneric()) {
        const auto& generics = function->GetGenericTypeParams();
        for (size_t i = 0; i < generics.size(); ++i) {
            if (generics[i] == type) {
                assembler.mov(target,
                    asmjit::x86::qword_ptr(asmjit::x86::rbp, context.GetHiddenTypeInfoOffset(i)));
                return;
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
                    JIT_ASSERT(parentType != nullptr && FindGenericPath(*parentType, *type, path) && !path.empty(),
                        "extended generic parameter resolves through the extended type");
                } else {
                    path.push_back(i);
                }
                assembler.mov(target,
                    asmjit::x86::qword_ptr(asmjit::x86::rbp,
                        context.GetHiddenTypeInfoOffset(generics.size())));
                for (const auto index : path) {
                    JIT_ASSERT(index <= static_cast<size_t>(std::numeric_limits<int32_t>::max()) / sizeof(uint64_t),
                        "generic path index is out of range");
                    // x86-64 native TypeInfo.typeArgs.
                    assembler.mov(target, asmjit::x86::qword_ptr(target, 48));
                    assembler.mov(target, asmjit::x86::qword_ptr(target, static_cast<int32_t>(index * sizeof(uint64_t))));
                }
                return;
            }
        }
        JIT_FAIL("generic type information is not available in this function");
    }
    if (type->IsThis()) {
        if (function->TestAttr(CHIR::Attribute::STATIC)) {
            const auto index = function->GetGenericTypeParams().size() +
                (function->GetParentCustomTypeDef() ? 1U : 0U);
            assembler.mov(target,
                asmjit::x86::qword_ptr(asmjit::x86::rbp, context.GetHiddenTypeInfoOffset(index)));
            return;
        }
        const auto& parameters = function->GetParams();
        auto location = context.GetValueLocation(parameters.front());
        MemoryUtils::Move(context, RegisterUtils::ToLocation(target), *location,
            *parameters.front()->GetType(), asmjit::x86::r11);
        assembler.mov(target, asmjit::x86::qword_ptr(target));
        assembler.shl(target, 16);
        assembler.shr(target, 16);
        return;
    }
    if (type->IsGenericRelated()) {
        auto prepared = PrepareTypeTemplate(context, *type);
        const auto& arguments = prepared.arguments;
        const auto stackSize = static_cast<uint32_t>((arguments.size() * sizeof(uint64_t) + 15) & ~size_t{15});
        assembler.sub(asmjit::x86::rsp, stackSize);
        for (size_t i = 0; i < arguments.size(); ++i) {
            JIT_ASSERT(arguments[i] != nullptr, "type template argument is present");
            LoadTypeInfo(*arguments[i], asmjit::x86::rax);
            assembler.mov(asmjit::x86::qword_ptr(asmjit::x86::rsp, static_cast<int32_t>(i * sizeof(uint64_t))),
                asmjit::x86::rax);
        }
        assembler.long_().mov(asmjit::x86::rdi, asmjit::imm(reinterpret_cast<uintptr_t>(prepared.typeTemplate)));
        assembler.mov(asmjit::x86::esi, static_cast<uint32_t>(arguments.size()));
        assembler.mov(asmjit::x86::rdx, asmjit::x86::rsp);
        assembler.long_().mov(asmjit::x86::r11, asmjit::imm(reinterpret_cast<uintptr_t>(prepared.create)));
        // Native CodeGen emits llvm.cj.get.type.info as a nounwind, readonly
        // intrinsic; its runtime helper is not a managed safepoint.
        assembler.call(asmjit::x86::r11);
        assembler.add(asmjit::x86::rsp, stackSize);
        if (target != asmjit::x86::rax) {
            assembler.mov(target, asmjit::x86::rax);
        }
        return;
    }
    auto info = GetConcreteTypeInfo(*type);
    JIT_ASSERT(info != nullptr, "concrete type information is available");
    assembler.long_().mov(target, asmjit::imm(reinterpret_cast<uintptr_t>(info)));
}
} // namespace Cangjie::JIT::X86_64
