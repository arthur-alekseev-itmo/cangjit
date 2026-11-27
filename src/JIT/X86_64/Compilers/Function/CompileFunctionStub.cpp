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
#include "../../JitAssert.h"
#include "../../../Runtime/FunctionMetadata.h"
#include "../../../Runtime/RuntimeError.h"
#include "../../Utils/StackFrameUtils.h"
#include "../../Utils/TypeUtils.h"
#include "../../Utils/SystemVCallingConvention.h"
#include "CompileFunction.h"
#include "cangjie/CHIR/IR/Annotation.h"
#include "cangjie/CHIR/IR/Type/Type.h"
#include "cangjie/CHIR/IR/Value/Value.h"

namespace Cangjie::JIT::X86_64 {
namespace {
using namespace asmjit;
using namespace asmjit::x86;


CodeAddress Materialize(const CHIR::Function& function, void* context)
{
    return CompileFunction(*static_cast<GlobalContext*>(context), function).CompileToAddress();
}

// Runs on the generated-code stack with the original arguments saved by the
// resolver stub; a materialization failure is reported and terminates.
CodeAddress ResolveAndPatch(GlobalContext* context, FunctionRecord* record, CodeAddress returnAddress) noexcept
{
    try {
        auto found = context->GetLazyCallSiteRegistry().Find(returnAddress);
        const auto site = found && found->callee == record ? std::optional<LazyCallSite>(*found) : std::nullopt;
        const auto target = context->GetFunctionResolver().Resolve(*record, Materialize, context);
        if (site) {
            auto allocator = context->GetRuntime().allocator();
            JitAllocator::Span span;
            const uint64_t patched = target;
            JIT_ASSERT(allocator->query(span, reinterpret_cast<void*>(site->caller)) == kErrorOk &&
                reinterpret_cast<CodeAddress>(span.rx()) == site->caller,
                "lazy call site lies in a block owned by the JIT allocator");
            JIT_ASSERT(allocator->write(span, site->immediateOffset, &patched, sizeof(patched)) == kErrorOk,
                "lazy call site is patched");
            context->GetLogger().LogCallPatched(record->GetFunction().GetIdentifier(), site->caller,
                site->caller + site->immediateOffset, target);
        }
        return target;
    } catch (const RuntimeError& error) {
        std::fprintf(stderr, "JIT lazy compilation failed: %s\n", FormatRuntimeError(error).c_str());
    } catch (const std::exception& error) {
        std::fprintf(stderr, "JIT lazy compilation failed: %s\n", error.what());
    } catch (...) {
        std::fputs("JIT lazy compilation failed with an unknown exception\n", stderr);
    }
    std::abort();
}

bool IsObjectReference(const CHIR::Type& type)
{
    if (!type.IsRef()) {
        return false;
    }
    auto base = static_cast<const CHIR::RefType&>(type).GetBaseType();
    return base && (base->IsClass() || base->IsRawArray() || base->IsBox());
}

// Collects the GC slots the resolver stub must report: reference arguments
// live in the saved-register frame while the runtime resolves the callee.
void CollectRootSlots(const CHIR::Function& function, std::vector<int32_t>& gcSlots)
{
    JIT_ASSERT(!function.IsCFunc(), "lazily compiled callee is not a C function");
    const auto placement = SystemVCallingConvention::GetNativeParameterPlacement(function);
    // sret returns only pass through Unit/Nothing; other aggregates are unsupported.
    JIT_ASSERT(placement.basePointerLocations.empty() && !placement.indirectReturn,
        "lazily compiled callee needs no base pointer or indirect return");
    const auto supported = [](const CHIR::Type& type) {
        return type.IsInteger() || type.IsBoolean() || TypeUtils::IsFloating(type) ||
            IsObjectReference(type);
    };
    auto result = placement.abiReturnType;
    JIT_ASSERT(result && (result->IsVoid() || result->IsUnit() || result->IsNothing() || supported(*result)),
        "lazily compiled callee has a supported return type");
    for (auto parameter : function.GetParams()) {
        const auto& type = placement.abiParameterTypes.at(parameter);
        const auto& location = placement.locations.at(parameter);
        JIT_ASSERT(type && supported(*type), "lazily compiled callee has supported parameter types");
        if (!IsObjectReference(*type)) {
            continue;
        }
        if (auto reg = location.AsGpRegister()) {
            JIT_ASSERT(reg->index < 6, "reference parameter arrives in an integer argument register");
            gcSlots.push_back(-16 - static_cast<int32_t>(reg->index * 8));
        } else {
            auto stack = location.AsIncomingStack();
            JIT_ASSERT(stack != nullptr, "reference parameter arrives in a register or on the stack");
            gcSlots.push_back(StackFrameUtils::GetIncomingArgumentOffset(stack->index));
        }
    }
}

class ResolverStubEmitter final {
public:
    explicit ResolverStubEmitter(Assembler& assembler) : a(assembler) {}

    void Emit(GlobalContext& context, FunctionRecord& record, void* c2nStub, uint32_t& safepoint)
    {
        SaveArguments();
        ResolveAndJump(context, record, c2nStub, safepoint);
    }

private:
    // Build a runtime frame and save the incoming integer and SIMD argument registers.
    void SaveArguments()
    {
        a.push(rbp);
        a.mov(rbp, rsp);
        const auto marker = a.newLabel();
        a.call(marker);
        a.bind(marker);
        // The marker occupies [rbp-8]. Keep RSP 16-byte aligned at the call.
        a.sub(rsp, 184);
        a.mov(qword_ptr(rbp, -16), rdi);
        a.mov(qword_ptr(rbp, -24), rsi);
        a.mov(qword_ptr(rbp, -32), rdx);
        a.mov(qword_ptr(rbp, -40), rcx);
        a.mov(qword_ptr(rbp, -48), r8);
        a.mov(qword_ptr(rbp, -56), r9);
        a.movdqu(xmmword_ptr(rbp, -72), xmm0);
        a.movdqu(xmmword_ptr(rbp, -88), xmm1);
        a.movdqu(xmmword_ptr(rbp, -104), xmm2);
        a.movdqu(xmmword_ptr(rbp, -120), xmm3);
        a.movdqu(xmmword_ptr(rbp, -136), xmm4);
        a.movdqu(xmmword_ptr(rbp, -152), xmm5);
        a.movdqu(xmmword_ptr(rbp, -168), xmm6);
        a.movdqu(xmmword_ptr(rbp, -184), xmm7);
    }

    // Resolve the target, restore the original arguments, then tail-jump to it.
    void ResolveAndJump(GlobalContext& context, FunctionRecord& record, void* c2nStub, uint32_t& safepoint)
    {
        a.mov(rdi, reinterpret_cast<uintptr_t>(&context));
        a.mov(rsi, reinterpret_cast<uintptr_t>(&record));
        a.mov(rdx, qword_ptr(rbp, 8));
        a.mov(rax, reinterpret_cast<uintptr_t>(&ResolveAndPatch));
        a.push(rax);
        a.push(0);
        a.mov(rax, reinterpret_cast<uintptr_t>(c2nStub));
        a.call(rax);
        // CJ_MCC_C2NStub removes both stack words before returning.
        safepoint = static_cast<uint32_t>(a.offset() - 4);
        a.mov(r11, rax);
        a.mov(rdi, qword_ptr(rbp, -16));
        a.mov(rsi, qword_ptr(rbp, -24));
        a.mov(rdx, qword_ptr(rbp, -32));
        a.mov(rcx, qword_ptr(rbp, -40));
        a.mov(r8, qword_ptr(rbp, -48));
        a.mov(r9, qword_ptr(rbp, -56));
        a.movdqu(xmm0, xmmword_ptr(rbp, -72));
        a.movdqu(xmm1, xmmword_ptr(rbp, -88));
        a.movdqu(xmm2, xmmword_ptr(rbp, -104));
        a.movdqu(xmm3, xmmword_ptr(rbp, -120));
        a.movdqu(xmm4, xmmword_ptr(rbp, -136));
        a.movdqu(xmm5, xmmword_ptr(rbp, -152));
        a.movdqu(xmm6, xmmword_ptr(rbp, -168));
        a.movdqu(xmm7, xmmword_ptr(rbp, -184));
        a.mov(rsp, rbp);
        a.pop(rbp);
        a.jmp(r11);
    }

    Assembler& a;
};

// Emits a per-callee entry that resolves the function on first call. The
// allocation starts with the metadata prefix; the executable entry follows it.
void* MakeResolverStub(GlobalContext& context, FunctionRecord& record, std::vector<int32_t> gcSlots)
{
    auto& runtime = context.GetRuntime();
    auto c2nStub = context.GetRuntimeBindings().GetCangjieToNativeStub();
    CodeHolder code;
    JIT_ASSERT(code.init(runtime.environment()) == kErrorOk, "resolver stub code holder initializes");
    context.GetLogger().LogResolverStubEmission(code, record.GetFunction().GetIdentifier());
    Assembler assembler(&code);
    JIT_ASSERT(assembler.embedUInt32(0) == kErrorOk, "resolver stub prefix is emitted");
    uint32_t safepoint = 0;
    ResolverStubEmitter(assembler).Emit(context, record, c2nStub, safepoint);
    // The prefix and the stub body are always emitted.
    const auto offset = assembler.offset();
    JIT_ASSERT(offset > 4 && offset - 4 <= UINT32_MAX, "resolver stub has a valid code size");
    const auto codeSize = static_cast<uint32_t>(offset - 4);
    FunctionMetadata metadata(context.IsStackGrowthEnabled());
    metadata.RecordFrame(192, {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX});
    // Reference arguments are the only roots; a stub never has stack-interior roots.
    metadata.RecordSafepoint(safepoint, 0, std::move(gcSlots), 0, {}, 0, {});
    const auto provisional = metadata.Serialize(0x20000000, 0x10000000, codeSize);
    JIT_ASSERT(assembler.align(AlignMode::kData, 4) == kErrorOk, "resolver stub metadata is aligned");
    const auto metadataOffset = assembler.offset();
    JIT_ASSERT(assembler.embed(provisional.descriptor.data(), provisional.descriptor.size()) == kErrorOk,
        "resolver stub metadata is embedded");
    void* allocation = nullptr;
    const auto allocationError = runtime.add(&allocation, &code);
    if (allocationError == kErrorOutOfMemory) {
        throw RuntimeError{RuntimeErrorKind::OutOfMemory, 0, "allocating a resolver stub"};
    }
    JIT_ASSERT(allocationError == kErrorOk, "resolver stub is allocated");
    const auto base = reinterpret_cast<CodeAddress>(allocation);
    const auto finalMetadata = metadata.Serialize(base + metadataOffset, base + 4, codeSize);
    // The descriptor layout depends on the code size only, never on addresses.
    JIT_ASSERT(finalMetadata.descriptor.size() == provisional.descriptor.size(),
        "resolver stub metadata size is address independent");
    JitAllocator::Span span;
    auto allocator = runtime.allocator();
    JIT_ASSERT(allocator->query(span, allocation) == kErrorOk, "resolver stub block is known to the allocator");
    JIT_ASSERT(allocator->write(span, metadataOffset, finalMetadata.descriptor.data(),
        finalMetadata.descriptor.size()) == kErrorOk, "resolver stub metadata is published");
    JIT_ASSERT(allocator->write(span, 0, finalMetadata.prefix.data(), finalMetadata.prefix.size()) == kErrorOk,
        "resolver stub prefix is published");
    return allocation;
}
} // namespace

// See ai-docs/jit-function-stub.md for the frame and resolver flow.
CodeAddress CompileFunctionStub::Compile(GlobalContext& context, const CHIR::Function* function)
{
    JIT_ASSERT(function != nullptr, "lazy compilation target is a function");
    auto& record = context.GetFunctionRegistry().GetOrCreate(*function);
    if (record.GetEntryStub() != 0) {
        return record.GetEntryStub();
    }
    std::vector<int32_t> gcSlots;
    CollectRootSlots(*function, gcSlots);
    void* allocation = MakeResolverStub(context, record, std::move(gcSlots));
    const auto entry = reinterpret_cast<CodeAddress>(allocation) + 4;
    record.Initialize(entry);
    context.GetLogger().LogResolverStubCreated(function->GetIdentifier(), entry);
    return entry;
}
} // namespace Cangjie::JIT::X86_64
