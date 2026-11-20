// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_FUNCTION_CONTEXT_H
#define CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_FUNCTION_CONTEXT_H

#include <asmjit/x86.h>

#include <iosfwd>
#include <unordered_map>
#include <vector>

#include "JitFailure.h"
#include "../../JitMetadataBuilder.h"
#include "../../LazyCalls/LazyCallSiteRegistry.h"
#include "../../RegisterAllocation/InterferenceGraph.h"
#include "../../RegisterAllocation/RegisterAllocation.h"
#include "../../RegisterAllocation/ValueNumeration.h"

namespace Cangjie::CHIR {
class Expression;
class Block;
class Function;
class Type;
class Value;
}

namespace Cangjie::JIT::X86_64 {
class GlobalContext;

class FunctionContext final {
public:
    FunctionContext(GlobalContext& globalContext, const CHIR::Function* function, ValueNumeration valueNumeration,
        InterferenceGraph interferenceGraph, RegisterAllocation registerAllocation) noexcept;

    GlobalContext& GetGlobalContext() const noexcept;
    asmjit::CodeHolder& GetCode() noexcept;
    asmjit::x86::Assembler& GetAssembler() noexcept;
    JitMetadataBuilder& GetMetadataBuilder() noexcept;
    std::optional<int32_t> GetHiddenTypeInfoOffset(size_t index) const noexcept;
    bool LoadTypeInfo(const CHIR::Type& type, const asmjit::x86::Gp& target);
    bool RecordSafepoint(const CHIR::Expression& expression, const CHIR::Value* temporaryRoot = nullptr);
    bool EmitExceptionLanding(const CHIR::Block& block, size_t callStart, size_t callEnd);
    asmjit::Error GetError() const noexcept;
    asmjit::Label GetBlockLabel(const CHIR::Block* block);
    asmjit::Label GetEpilogueLabel() const noexcept;
    void Fail(JitFailureKind kind, std::optional<CHIR::ExprKind> expressionKind = std::nullopt) noexcept;
    const std::optional<JitFailure>& GetFailure() const noexcept;
    const CHIR::Function* GetFunction() const noexcept;
    std::vector<const CHIR::Function*>& GetFunctionsToStub() noexcept;
    std::vector<PendingLazyCallSite>& GetLazyCallSites() noexcept;
    const std::vector<const CHIR::Function*>& GetFunctionsToStub() const noexcept;
    const ValueNumeration& GetValueNumeration() const noexcept;
    const InterferenceGraph& GetInterferenceGraph() const noexcept;
    const RegisterAllocation& GetRegisterAllocation() const noexcept;
    void Dump(std::ostream& output) const;

private:
    class EmitterErrorHandler final : public asmjit::ErrorHandler {
    public:
        void handleError(asmjit::Error emittedError, const char*, asmjit::BaseEmitter*) override
        {
            if (firstError == asmjit::kErrorOk) {
                firstError = emittedError;
            }
        }

        asmjit::Error firstError = asmjit::kErrorOk;
    };

    GlobalContext& globalContext;
    EmitterErrorHandler emitterErrorHandler;
    asmjit::CodeHolder code;
    asmjit::x86::Assembler assembler;
    asmjit::Error error;
    JitMetadataBuilder metadataBuilder;
    std::optional<JitFailure> failure;
    asmjit::Label epilogueLabel;
    std::unordered_map<const CHIR::Block*, asmjit::Label> blockLabels;
    const CHIR::Function* function;
    std::vector<const CHIR::Function*> functionsToStub;
    std::vector<PendingLazyCallSite> lazyCallSites;
    ValueNumeration valueNumeration;
    InterferenceGraph interferenceGraph;
    RegisterAllocation registerAllocation;
    size_t hiddenTypeInfoBase = 0;
    size_t hiddenTypeInfoCount = 0;
};

} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_FUNCTION_CONTEXT_H
