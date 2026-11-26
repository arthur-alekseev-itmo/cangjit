// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "ExceptionLandingEmitter.h"

#include <limits>
#include <utility>

#include "FunctionContext.h"
#include "GlobalContext.h"
#include "../../Utils/StackFrameUtils.h"
#include "cangjie/CHIR/IR/Value/Value.h"
#include "cangjie/CHIR/IR/Type/ClassDef.h"
#include "cangjie/CHIR/IR/Type/Type.h"
#include "cangjie/Mangle/CHIRTypeManglingUtils.h"

namespace Cangjie::JIT::X86_64 {
std::optional<std::vector<uintptr_t>> ExceptionLandingEmitter::GetExceptionTypes(const CHIR::Block& block)
{
    const auto exceptions = block.GetExceptions();
    if (exceptions.empty()) {
        return std::vector<uintptr_t>{0};
    }
    auto& bindings = context.GetGlobalContext().GetRuntimeBindings();
    auto symbol = bindings.GetTypeByName();
    if (!symbol) {
        context.Fail(JitFailureKind::UnresolvedSymbol);
        return std::nullopt;
    }
    using LookupType = void* (*)(const char*);
    const auto lookup = reinterpret_cast<LookupType>(symbol.Value());
    std::vector<uintptr_t> types;
    types.reserve(exceptions.size());
    for (const auto* type : exceptions) {
        if (!type) {
            context.Fail(JitFailureKind::UnsupportedExpression);
            return std::nullopt;
        }
        const auto* def = type->GetClassDef();
        if (!def || !bindings.EnsurePackage(def->GetPackageName())) {
            context.Fail(JitFailureKind::UnresolvedSymbol);
            return std::nullopt;
        }
        const auto name = CHIR::GetTypeQualifiedName(*type, true);
        auto* info = lookup(name.c_str());
        if (!info) {
            context.Fail(JitFailureKind::UnresolvedSymbol);
            return std::nullopt;
        }
        types.push_back(reinterpret_cast<uintptr_t>(info));
    }
    return types;
}

bool ExceptionLandingEmitter::Emit(const CHIR::Block& block, size_t callStart, size_t callEnd)
{
    auto& assembler = context.GetAssembler();
    const auto landingOffset = assembler.offset();
    if (!block.IsLandingPadBlock() || callStart < 4 || callEnd <= callStart ||
        callEnd - 4 > std::numeric_limits<uint32_t>::max() ||
        landingOffset < 4 || landingOffset - 4 > std::numeric_limits<uint32_t>::max()) {
        context.Fail(JitFailureKind::UnsupportedExpression);
        return false;
    }
    auto types = GetExceptionTypes(block);
    if (!types) {
        return false;
    }
    const auto slots = context.GetRegisterAllocation().GetStackSlotCount();
    const auto frameSize = StackFrameUtils::GetFrameSize(slots + 1);
    if (!frameSize) {
        context.Fail(JitFailureKind::StackFrameTooLarge);
        return false;
    }
    auto& metadata = context.GetMetadata();
    const auto landing = metadata.RecordLandingPad(static_cast<uint32_t>(landingOffset - 4), std::move(*types));
    metadata.RecordCallSite(static_cast<uint32_t>(callStart - 4), static_cast<uint32_t>(callEnd - callStart), landing);
    assembler.lea(asmjit::x86::rsp, asmjit::x86::ptr(asmjit::x86::rbp, -static_cast<int32_t>(*frameSize)));
    assembler.jmp(context.GetBlockLabel(&block));
    return context.GetError() == asmjit::kErrorOk;
}
} // namespace Cangjie::JIT::X86_64
