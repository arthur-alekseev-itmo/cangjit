// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "ExceptionLandingEmitter.h"

#include <utility>

#include "FunctionContext.h"
#include "GlobalContext.h"
#include "../../JitAssert.h"
#include "../../../Runtime/RuntimeError.h"
#include "../../Utils/StackFrameUtils.h"
#include "cangjie/CHIR/IR/Value/Value.h"
#include "cangjie/CHIR/IR/Type/ClassDef.h"
#include "cangjie/CHIR/IR/Type/Type.h"
#include "cangjie/Mangle/CHIRTypeManglingUtils.h"

namespace Cangjie::JIT::X86_64 {
std::vector<uintptr_t> ExceptionLandingEmitter::GetExceptionTypes(const CHIR::Block& block)
{
    const auto exceptions = block.GetExceptions();
    if (exceptions.empty()) {
        return {0};
    }
    auto& bindings = context.GetGlobalContext().GetRuntimeBindings();
    using LookupType = void* (*)(const char*);
    const auto lookup = reinterpret_cast<LookupType>(bindings.GetTypeByName());
    std::vector<uintptr_t> types;
    types.reserve(exceptions.size());
    for (auto type : exceptions) {
        JIT_ASSERT(type != nullptr, "exception type is required");
        auto def = type->GetClassDef();
        JIT_ASSERT(def != nullptr, "exception type has a class definition");
        Require(bindings.EnsurePackage(def->GetPackageName()));
        const auto name = CHIR::GetTypeQualifiedName(*type, true);
        auto info = lookup(name.c_str());
        JIT_ASSERT(info != nullptr, "exception type information is missing from loaded libraries");
        types.push_back(reinterpret_cast<uintptr_t>(info));
    }
    return types;
}

void ExceptionLandingEmitter::Emit(const CHIR::Block& block, size_t callStart, size_t callEnd)
{
    auto& assembler = context.GetAssembler();
    const auto landingOffset = assembler.offset();
    JIT_ASSERT(block.IsLandingPadBlock() && callStart >= 4 && callEnd > callStart,
        "landing pad or call site is valid");
    auto types = GetExceptionTypes(block);
    const auto slots = context.GetRegisterAllocation().GetStackSlotCount();
    const auto frameSize = StackFrameUtils::GetFrameSize(slots + 1);
    auto& metadata = context.GetMetadata();
    const auto landing = metadata.RecordLandingPad(static_cast<uint32_t>(landingOffset - 4), std::move(types));
    metadata.RecordCallSite(static_cast<uint32_t>(callStart - 4), static_cast<uint32_t>(callEnd - callStart), landing);
    assembler.lea(asmjit::x86::rsp, asmjit::x86::ptr(asmjit::x86::rbp, -static_cast<int32_t>(frameSize)));
    assembler.jmp(context.GetBlockLabel(&block));
}
} // namespace Cangjie::JIT::X86_64
