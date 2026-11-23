// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_FUNCTION_CONTEXT_H
#define CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_FUNCTION_CONTEXT_H

#include <asmjit/x86.h>

#include <unordered_map>
#include <vector>

#include "JitFailure.h"
#include "ExceptionLandingEmitter.h"
#include "LocalFinalizers.h"
#include "SafepointRecorder.h"
#include "TypeInfoEmitter.h"
#include "../../JitMetadataBuilder.h"
#include "../../LazyCalls/LazyCallSiteRegistry.h"
#include "../../RegisterAllocation/LivenessAnalysis.h"
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

// Per-function JIT state: emitted code, allocation, metadata, and diagnostics.
// Also tracks labels and pending work while the function is compiled.
class FunctionContext final {
public:
    FunctionContext(GlobalContext& globalContext, const CHIR::Function* function, ValueNumeration valueNumeration,
        LivenessAnalysis liveness, RegisterAllocation registerAllocation) noexcept;
    bool Prepare();

    GlobalContext& GetGlobalContext() const noexcept;
    asmjit::CodeHolder& GetCode() noexcept;
    asmjit::x86::Assembler& GetAssembler() noexcept;
    JitMetadataBuilder& GetMetadataBuilder() noexcept;

    std::optional<int32_t> GetHiddenTypeInfoOffset(size_t index) const noexcept;
    TypeInfoEmitter& GetTypeInfoEmitter() noexcept { return typeInfoEmitter; }
    SafepointRecorder& GetSafepointRecorder() noexcept { return safepointRecorder; }
    ExceptionLandingEmitter& GetExceptionLandingEmitter() noexcept { return exceptionLandingEmitter; }
    asmjit::Error GetError() const noexcept;
    LocalFinalizers& GetLocalFinalizers() noexcept { return localFinalizers; }
    asmjit::Label GetBlockLabel(const CHIR::Block* block);
    asmjit::Label GetEpilogueLabel() const noexcept;
    void Fail(JitFailureKind kind, std::optional<CHIR::ExprKind> expressionKind = std::nullopt) noexcept;
    const std::optional<JitFailure>& GetFailure() const noexcept;
    const CHIR::Function* GetFunction() const noexcept;
    std::vector<const CHIR::Function*>& GetFunctionsToStub() noexcept;
    std::vector<PendingLazyCallSite>& GetLazyCallSites() noexcept;
    const std::vector<const CHIR::Function*>& GetFunctionsToStub() const noexcept;
    const ValueNumeration& GetValueNumeration() const noexcept;
    const LivenessAnalysis& GetLivenessAnalysis() const noexcept { return liveness; }
    const RegisterAllocation& GetRegisterAllocation() const noexcept;

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
    void ReserveHiddenTypeInfoSlots() noexcept;
    bool InitializeAssembler();


    GlobalContext& globalContext;
    EmitterErrorHandler emitterErrorHandler;
    asmjit::CodeHolder code;
    asmjit::x86::Assembler assembler;
    asmjit::Error error;
    JitMetadataBuilder metadataBuilder;
    std::optional<JitFailure> failure;
    SafepointRecorder safepointRecorder{*this};
    ExceptionLandingEmitter exceptionLandingEmitter{*this};
    TypeInfoEmitter typeInfoEmitter{*this};
    LocalFinalizers localFinalizers{*this};
    asmjit::Label epilogueLabel;
    std::unordered_map<const CHIR::Block*, asmjit::Label> blockLabels;
    const CHIR::Function* function;
    std::vector<const CHIR::Function*> functionsToStub;
    std::vector<PendingLazyCallSite> lazyCallSites;
    ValueNumeration valueNumeration;
    LivenessAnalysis liveness;
    RegisterAllocation registerAllocation;
    size_t hiddenTypeInfoBase = 0;
    size_t hiddenTypeInfoCount = 0;
};

} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_FUNCTION_CONTEXT_H
