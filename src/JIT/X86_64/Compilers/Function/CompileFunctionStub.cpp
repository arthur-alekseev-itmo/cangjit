// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "CompileFunctionStub.h"

#include <asmjit/x86.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <vector>

#include "../Context/GlobalContext.h"
#include "../../../Runtime/JitMetadataBuilder.h"
#include "../../Utils/StackFrameUtils.h"
#include "../../Utils/TypeUtils.h"
#include "../../Utils/SystemVCallingConvention.h"
#include "CompileFunction.h"
#include "cangjie/CHIR/IR/Annotation.h"
#include "cangjie/CHIR/IR/Type/Type.h"
#include "cangjie/CHIR/IR/Value/Value.h"
#include "cangjie/Utils/CheckUtils.h"

namespace Cangjie::JIT::X86_64 {
namespace {
using namespace asmjit;
using namespace asmjit::x86;


CompilationResult Materialize(const CHIR::Function& function, void* context)
{
    return CompileFunction(*static_cast<GlobalContext*>(context), function).CompileToAddress();
}

CodeAddress ResolveAndPatch(GlobalContext* context, FunctionRecord* record, CodeAddress returnAddress) noexcept
{
    try {
        const auto* found = context->GetLazyCallSiteRegistry().Find(returnAddress);
        const auto site = found && found->callee == record ? std::optional<LazyCallSite>(*found) : std::nullopt;
        auto result = context->GetFunctionResolver().Resolve(*record, Materialize, context);
        if (result) {
            if (site) {
                auto* allocator = context->GetRuntime().allocator();
                JitAllocator::Span span;
                const uint64_t target = result.Value();
                if (allocator->query(span, reinterpret_cast<void*>(site->caller)) != kErrorOk ||
                    reinterpret_cast<CodeAddress>(span.rx()) != site->caller ||
                    allocator->write(span, site->immediateOffset, &target, sizeof(target)) != kErrorOk) {
                    std::fputs("JIT failed to patch lazy call site\n", stderr);
                    std::abort();
                }
                context->GetLogger().LogCallPatched(record->GetFunction().GetIdentifier(), site->caller,
                    site->caller + site->immediateOffset, result.Value());
            }
            return result.Value();
        }
        std::fprintf(stderr, "JIT lazy compilation failed: %s\n", result.Error().message.c_str());
    } catch (...) {
        std::fputs("JIT lazy compilation failed with an exception\n", stderr);
    }
    std::abort();
}

bool IsObjectReference(const CHIR::Type& type)
{
    if (!type.IsRef()) {
        return false;
    }
    const auto* base = static_cast<const CHIR::RefType&>(type).GetBaseType();
    return base && (base->IsClass() || base->IsRawArray() || base->IsBox());
}

bool SupportedSignature(const CHIR::Function& function, std::vector<int32_t>& gcSlots,
    std::vector<int32_t>& stackSlots)
{
    if (function.IsCFunc()) {
        return false;
    }
    const auto placement = SystemVCallingConvention::GetNativeParameterPlacement(function);
    if (!placement || !placement->basePointerLocations.empty() ||
        (placement->indirectReturn && !placement->abiReturnType->IsUnit() &&
            !placement->abiReturnType->IsNothing())) {
        return false;
    }
    const auto supported = [](const CHIR::Type& type) {
        return type.IsInteger() || type.IsBoolean() || TypeUtils::IsFloating(type) ||
            IsObjectReference(type);
    };
    const auto* result = placement->abiReturnType;
    if (!result || (!result->IsVoid() && !result->IsUnit() && !result->IsNothing() && !supported(*result))) {
        return false;
    }
    if (placement->indirectReturn) {
        const auto* reg = placement->sretLocation ? placement->sretLocation->AsRegister() : nullptr;
        if (!reg || reg->type != RegisterType::RDI || reg->index != 0) {
            return false;
        }
        stackSlots.push_back(-16);
    }
    for (const auto* parameter : function.GetParams()) {
        const auto type = placement->abiParameterTypes.find(parameter);
        const auto location = placement->locations.find(parameter);
        CJC_ASSERT(type != placement->abiParameterTypes.end());
        CJC_ASSERT(location != placement->locations.end());
        if (!parameter || type == placement->abiParameterTypes.end() || !type->second ||
            location == placement->locations.end() || !supported(*type->second)) {
            return false;
        }
        if (!IsObjectReference(*type->second)) {
            continue;
        }
        if (const auto* reg = location->second.AsRegister()) {
            if (reg->index >= 6) {
                return false;
            }
            gcSlots.push_back(-16 - static_cast<int32_t>(reg->index * 8));
        } else if (const auto* stack = location->second.AsIncomingStack()) {
            const auto offset = StackFrameUtils::GetIncomingArgumentOffset(stack->index);
            if (!offset) {
                return false;
            }
            gcSlots.push_back(*offset);
        } else {
            return false;
        }
    }
    return true;
}

class ResolverStubEmitter final {
public:
    explicit ResolverStubEmitter(Assembler& assembler) : a(assembler) {}

    bool Emit(GlobalContext& context, FunctionRecord& record, void* c2nStub, uint32_t& safepoint)
    {
        SaveArguments();
        ResolveAndJump(context, record, c2nStub, safepoint);
        return ok;
    }

private:
    // ok = ok && error == kErrorOk;
    void Emit(Error error)
    {
        ok = ok && error == kErrorOk;
    }

    // Build a runtime frame and save the incoming integer and SIMD argument registers.
    void SaveArguments()
    {
        Emit(a.push(rbp));
        Emit(a.mov(rbp, rsp));
        const auto marker = a.newLabel();
        Emit(a.call(marker));
        Emit(a.bind(marker));
        // The marker occupies [rbp-8]. Keep RSP 16-byte aligned at the call.
        Emit(a.sub(rsp, 184));
        Emit(a.mov(qword_ptr(rbp, -16), rdi));
        Emit(a.mov(qword_ptr(rbp, -24), rsi));
        Emit(a.mov(qword_ptr(rbp, -32), rdx));
        Emit(a.mov(qword_ptr(rbp, -40), rcx));
        Emit(a.mov(qword_ptr(rbp, -48), r8));
        Emit(a.mov(qword_ptr(rbp, -56), r9));
        Emit(a.movdqu(xmmword_ptr(rbp, -72), xmm0));
        Emit(a.movdqu(xmmword_ptr(rbp, -88), xmm1));
        Emit(a.movdqu(xmmword_ptr(rbp, -104), xmm2));
        Emit(a.movdqu(xmmword_ptr(rbp, -120), xmm3));
        Emit(a.movdqu(xmmword_ptr(rbp, -136), xmm4));
        Emit(a.movdqu(xmmword_ptr(rbp, -152), xmm5));
        Emit(a.movdqu(xmmword_ptr(rbp, -168), xmm6));
        Emit(a.movdqu(xmmword_ptr(rbp, -184), xmm7));
    }

    // Resolve the target, restore the original arguments, then tail-jump to it.
    void ResolveAndJump(GlobalContext& context, FunctionRecord& record, void* c2nStub, uint32_t& safepoint)
    {
        Emit(a.mov(rdi, reinterpret_cast<uintptr_t>(&context)));
        Emit(a.mov(rsi, reinterpret_cast<uintptr_t>(&record)));
        Emit(a.mov(rdx, qword_ptr(rbp, 8)));
        Emit(a.mov(rax, reinterpret_cast<uintptr_t>(&ResolveAndPatch)));
        Emit(a.push(rax));
        Emit(a.push(0));
        Emit(a.mov(rax, reinterpret_cast<uintptr_t>(c2nStub)));
        Emit(a.call(rax));
        // CJ_MCC_C2NStub removes both stack words before returning.
        safepoint = static_cast<uint32_t>(a.offset() - 4);
        Emit(a.mov(r11, rax));
        Emit(a.mov(rdi, qword_ptr(rbp, -16)));
        Emit(a.mov(rsi, qword_ptr(rbp, -24)));
        Emit(a.mov(rdx, qword_ptr(rbp, -32)));
        Emit(a.mov(rcx, qword_ptr(rbp, -40)));
        Emit(a.mov(r8, qword_ptr(rbp, -48)));
        Emit(a.mov(r9, qword_ptr(rbp, -56)));
        Emit(a.movdqu(xmm0, xmmword_ptr(rbp, -72)));
        Emit(a.movdqu(xmm1, xmmword_ptr(rbp, -88)));
        Emit(a.movdqu(xmm2, xmmword_ptr(rbp, -104)));
        Emit(a.movdqu(xmm3, xmmword_ptr(rbp, -120)));
        Emit(a.movdqu(xmm4, xmmword_ptr(rbp, -136)));
        Emit(a.movdqu(xmm5, xmmword_ptr(rbp, -152)));
        Emit(a.movdqu(xmm6, xmmword_ptr(rbp, -168)));
        Emit(a.movdqu(xmm7, xmmword_ptr(rbp, -184)));
        Emit(a.mov(rsp, rbp));
        Emit(a.pop(rbp));
        Emit(a.jmp(r11));
    }

    Assembler& a;
    bool ok = true;
};

void* MakeResolverStub(GlobalContext& context, FunctionRecord& record, std::vector<int32_t> gcSlots,
    std::vector<int32_t> stackSlots)
{
    auto symbol = context.GetRuntimeBindings().GetCangjieToNativeStub();
    if (!symbol) {
        return nullptr;
    }
    auto& runtime = context.GetRuntime();
    CodeHolder code;
    if (code.init(runtime.environment()) != kErrorOk) {
        return nullptr;
    }
    context.GetLogger().LogResolverStubEmission(code, record.GetFunction().GetIdentifier());
    Assembler assembler(&code);
    uint32_t safepoint = 0;
    if (assembler.embedUInt32(0) != kErrorOk ||
        !ResolverStubEmitter(assembler).Emit(context, record, symbol.Value(), safepoint) ||
        assembler.offset() <= 4 || assembler.offset() - 4 > UINT32_MAX) {
        return nullptr;
    }
    const auto codeSize = static_cast<uint32_t>(assembler.offset() - 4);
    JitMetadataBuilder metadata(context.IsStackGrowthEnabled());
    metadata.RecordFrame(192, {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX});
    if (!context.IsStackGrowthEnabled()) {
        stackSlots.clear();
    }
    metadata.RecordSafepoint(safepoint, 0, std::move(gcSlots), 0, std::move(stackSlots), 0, {});
    auto provisional = metadata.Build(0x20000000, 0x10000000, codeSize);
    if (!provisional || assembler.align(AlignMode::kData, 4) != kErrorOk) {
        return nullptr;
    }
    const auto metadataOffset = assembler.offset();
    if (assembler.embed(provisional.Value().descriptor.data(), provisional.Value().descriptor.size()) != kErrorOk) {
        return nullptr;
    }
    void* allocation = nullptr;
    if (runtime.add(&allocation, &code) != kErrorOk) {
        return nullptr;
    }
    const auto base = reinterpret_cast<CodeAddress>(allocation);
    auto finalMetadata = metadata.Build(base + metadataOffset, base + 4, codeSize);
    JitAllocator::Span span;
    auto* allocator = runtime.allocator();
    if (!finalMetadata || finalMetadata.Value().descriptor.size() != provisional.Value().descriptor.size() ||
        allocator->query(span, allocation) != kErrorOk ||
        allocator->write(span, metadataOffset, finalMetadata.Value().descriptor.data(),
            finalMetadata.Value().descriptor.size()) != kErrorOk ||
        allocator->write(span, 0, finalMetadata.Value().prefix.data(), finalMetadata.Value().prefix.size()) != kErrorOk) {
        runtime.release(allocation);
        return nullptr;
    }
    return allocation;
}
} // namespace

// See ai-docs/jit-function-stub.md for the frame and resolver flow.
CodeAddress CompileFunctionStub::Compile(GlobalContext& context, const CHIR::Function* function)
{
    std::vector<int32_t> gcSlots;
    std::vector<int32_t> stackSlots;
    if (!function || !SupportedSignature(*function, gcSlots, stackSlots)) {
        return 0;
    }
    auto& record = context.GetFunctionRegistry().GetOrCreate(*function);
    if (record.GetEntryStub() != 0) {
        return record.GetEntryStub();
    }

    // The allocation starts with the metadata prefix; the executable entry follows it.
    void* allocation = MakeResolverStub(context, record, std::move(gcSlots), std::move(stackSlots));
    if (!allocation) {
        return 0;
    }
    const auto entry = reinterpret_cast<CodeAddress>(allocation) + 4;
    if (!record.Initialize(entry)) {
        context.GetRuntime().release(allocation);
        return 0;
    }
    context.GetLogger().LogResolverStubCreated(function->GetIdentifier(), entry);
    return entry;
}
} // namespace Cangjie::JIT::X86_64
