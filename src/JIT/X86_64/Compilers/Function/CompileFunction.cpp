// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "CompileFunction.h"

#include <iostream>
#include <limits>
#include <utility>

#include "../../RegisterAllocation/LivenessInterferenceGraphBuilder.h"
#include "../../RegisterAllocation/TrivialStackRegalloc.h"
#include "../../RegisterAllocation/ValueNumeration.h"
#include "../../Utils/SystemVCallingConvention.h"
#include "../../Utils/MemoryUtils.h"
#include "../../Utils/RegisterUtils.h"
#include "../../Utils/StackFrameUtils.h"
#include "CompileBlock.h"
#include "../Context/GlobalContext.h"
#include "../Context/FunctionContext.h"
#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Type/Type.h"
#include "cangjie/CHIR/IR/Value/Value.h"

namespace Cangjie::JIT::X86_64 {

CompileFunction::CompileFunction(GlobalContext& globalContext, const CHIR::Function& function) noexcept
    : globalContext(globalContext), function(function)
{
}


CompilationResult CompileFunction::CompileToAddress(std::ostream* output) const
{
    if (!function.GetBody() || !function.GetBody()->GetEntryBlock() || !function.GetReturnType() ||
        !function.GetFuncType()) {
        return CompilationResult::Failure({"invalid function"});
    }
    if (auto* logger = globalContext.GetLogger()) {
        logger->logf("[JIT] compiling function %s\n", function.GetIdentifier().c_str());
    }
    auto valueNumeration = ValueNumerator(function).Run();
    auto liveness = LivenessAnalysis::Analyze(function, valueNumeration);
    auto interferenceGraph = LivenessInterferenceGraphBuilder().Build(function, valueNumeration, liveness);
    auto registerAllocation = TrivialStackRegalloc().Allocate(valueNumeration, interferenceGraph);
    FunctionContext context(globalContext, &function, std::move(valueNumeration), std::move(liveness),
        std::move(interferenceGraph), std::move(registerAllocation));
    if (context.Prepare()) {
        if (output) {
            context.Dump(*output);
            if (!*output) {
                return CompilationResult::Failure({"failed to write JIT dump"});
            }
        }
        Compile(context);
    }
    if (const auto& failure = context.GetFailure()) {
        std::string message = GetJitFailureMessage(failure->kind);
        if (failure->expressionKind) {
            message += ": ";
            message += CHIR::ExprKindMgr::Instance()->GetKindName(static_cast<size_t>(*failure->expressionKind));
        }
        return CompilationResult::Failure({std::move(message)});
    }
    if (context.GetError() != asmjit::kErrorOk) {
        return CompilationResult::Failure({asmjit::DebugUtils::errorAsString(context.GetError())});
    }
    auto& assembler = context.GetAssembler();
    constexpr size_t PREFIX_SIZE = sizeof(uint32_t);
    if (assembler.offset() <= PREFIX_SIZE || assembler.offset() - PREFIX_SIZE > UINT32_MAX) {
        return CompilationResult::Failure({"invalid JIT code size"});
    }
    const auto codeSize = static_cast<uint32_t>(assembler.offset() - PREFIX_SIZE);
    const auto slots = context.GetRegisterAllocation().GetStackSlotCount();
    const auto frameSize = StackFrameUtils::GetFrameSize(slots + 1);
    if (!frameSize) {
        return CompilationResult::Failure({"JIT stack frame is too large"});
    }
    // Frame and descriptor publication are described in ai-docs/jit-function-frame-and-metadata.md.
    context.GetMetadataBuilder().RecordFrame(*frameSize, {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX});
    // The metadata is emitted in the same allocation as the code so that all
    // descriptor-relative offsets are representable and have the same lifetime.
    constexpr uintptr_t PROVISIONAL_CODE = 0x10000000;
    constexpr uintptr_t PROVISIONAL_METADATA = 0x20000000;
    auto provisional = context.GetMetadataBuilder().Build(PROVISIONAL_METADATA, PROVISIONAL_CODE, codeSize);
    if (!provisional) {
        return CompilationResult::Failure({provisional.Error()});
    }
    assembler.align(asmjit::AlignMode::kData, 4);
    const size_t metadataOffset = assembler.offset();
    const auto& bytes = provisional.Value().descriptor;
    assembler.embed(bytes.data(), bytes.size());
    if (context.GetError() != asmjit::kErrorOk) {
        return CompilationResult::Failure({asmjit::DebugUtils::errorAsString(context.GetError())});
    }
    void* address = nullptr;
    const auto error = globalContext.GetRuntime().add(&address, &context.GetCode());
    if (error != asmjit::kErrorOk) {
        return CompilationResult::Failure({asmjit::DebugUtils::errorAsString(error)});
    }
    const auto base = reinterpret_cast<CodeAddress>(address);
    auto metadata = context.GetMetadataBuilder().Build(base + metadataOffset, base + PREFIX_SIZE, codeSize);
    if (!metadata) {
        globalContext.GetRuntime().release(address);
        return CompilationResult::Failure({metadata.Error()});
    }
    const auto& encoded = metadata.Value();
    asmjit::JitAllocator::Span span;
    auto* allocator = globalContext.GetRuntime().allocator();
    if (encoded.descriptor.size() != bytes.size() || allocator->query(span, address) != asmjit::kErrorOk ||
        allocator->write(span, metadataOffset, encoded.descriptor.data(), encoded.descriptor.size()) != asmjit::kErrorOk ||
        allocator->write(span, 0, encoded.prefix.data(), encoded.prefix.size()) != asmjit::kErrorOk) {
        globalContext.GetRuntime().release(address);
        return CompilationResult::Failure({"failed to publish JIT metadata"});
    }
    if (!globalContext.GetLazyCallSiteRegistry().Register(base, context.GetCode().codeSize(), context.GetLazyCallSites())) {
        globalContext.GetRuntime().release(address);
        return CompilationResult::Failure({"failed to register lazy call sites"});
    }
    if (auto* logger = globalContext.GetLogger()) {
        logger->logf("[JIT] function created name=%s entry=%p code_size=%zu frame_size=%zu "
                     "metadata=%p metadata_size=%zu\n",
            function.GetIdentifier().c_str(), reinterpret_cast<void*>(base + PREFIX_SIZE),
            static_cast<size_t>(codeSize), static_cast<size_t>(*frameSize),
            reinterpret_cast<void*>(base + metadataOffset), encoded.descriptor.size());
    }
    return CompilationResult::Success(base + PREFIX_SIZE);
}

void CompileFunction::MakePrologue(FunctionContext& context) const
{
    const auto slots = context.GetRegisterAllocation().GetStackSlotCount();
    const auto size = StackFrameUtils::GetFrameSize(slots + 1);
    if (!size) {
        context.Fail(JitFailureKind::StackFrameTooLarge);
        return;
    }
    auto& assembler = context.GetAssembler();
    // The runtime recovers the entry from the return address at [rbp - 8].
    // push (1 byte), mov (3 bytes), and the relative call (5 bytes) make
    // that address exactly entry + 9, as required by FrameInfo.
    const auto marker = assembler.newLabel();
    assembler.push(asmjit::x86::rbp);
    assembler.mov(asmjit::x86::rbp, asmjit::x86::rsp);
    assembler.call(marker);
    assembler.bind(marker);
    if (*size > sizeof(uint64_t)) {
        assembler.sub(asmjit::x86::rsp, *size - sizeof(uint64_t));
    }
}

void CompileFunction::MakeEpilogue(FunctionContext& context) const
{
    auto& assembler = context.GetAssembler();
    assembler.bind(context.GetEpilogueLabel());
    if (!SaveReturnValue(context)) {
        return;
    }
    assembler.mov(asmjit::x86::rsp, asmjit::x86::rbp);
    assembler.pop(asmjit::x86::rbp);
    assembler.ret();
}

bool CompileFunction::SaveReturnValue(FunctionContext& context) const
{
    const auto* type = function.GetReturnType();
    if (type->IsUnit() || type->IsVoid() || type->IsNothing()) {
        return true;
    }
    const auto kind = type->GetTypeKind();
    const auto width = type->IsBoolean() ? 8U : RegisterUtils::IntegerWidth(*type);
    const bool pointer = type->IsRef() || type->IsCPointer() || type->IsCString();
    if (!width && !pointer && kind != CHIR::Type::TYPE_FLOAT32 && kind != CHIR::Type::TYPE_FLOAT64) {
        context.Fail(JitFailureKind::UnsupportedReturnType);
        return false;
    }
    if (!MemoryUtils::LoadValue(context, function.GetReturnValue(), asmjit::x86::rax)) {
        return false;
    }
    LoadReturnValueFromAddress(context, *type, width);
    return true;
}

void CompileFunction::LoadReturnValueFromAddress(
    FunctionContext& context, const CHIR::Type& type, unsigned width) const
{
    auto& assembler = context.GetAssembler();
    const auto kind = type.GetTypeKind();
    if (kind == CHIR::Type::TYPE_FLOAT32) {
        assembler.movss(asmjit::x86::xmm0, asmjit::x86::dword_ptr(asmjit::x86::rax));
    } else if (kind == CHIR::Type::TYPE_FLOAT64) {
        assembler.movsd(asmjit::x86::xmm0, asmjit::x86::qword_ptr(asmjit::x86::rax));
    } else if (width == 8) {
        assembler.movzx(asmjit::x86::eax, asmjit::x86::byte_ptr(asmjit::x86::rax));
        RegisterUtils::NormalizeInteger(assembler, asmjit::x86::rax, width, type.IsSignedInteger());
    } else if (width == 16) {
        assembler.movzx(asmjit::x86::eax, asmjit::x86::word_ptr(asmjit::x86::rax));
        RegisterUtils::NormalizeInteger(assembler, asmjit::x86::rax, width, type.IsSignedInteger());
    } else if (width == 32) {
        assembler.mov(asmjit::x86::eax, asmjit::x86::dword_ptr(asmjit::x86::rax));
        RegisterUtils::NormalizeInteger(assembler, asmjit::x86::rax, width, type.IsSignedInteger());
    } else {
        assembler.mov(asmjit::x86::rax, asmjit::x86::qword_ptr(asmjit::x86::rax));
    }
}

// TODO: Re-review this function after the helper extraction.
void CompileFunction::Compile(FunctionContext& context) const
{
    if (context.GetFunction() != &function || !function.GetBody() || !function.GetBody()->GetEntryBlock()) {
        context.Fail(JitFailureKind::InvalidFunctionContext);
        return;
    }
    const auto placements = SystemVCallingConvention::GetNativeParameterPlacement(function);
    if (!placements || !placements->basePointerLocations.empty() ||
        (placements->indirectReturn && !placements->abiReturnType->IsUnit() &&
            !placements->abiReturnType->IsNothing())) {
        context.Fail(JitFailureKind::UnsupportedCallingConvention);
        return;
    }
    if (!context.GetLocalFinalizers().Prepare()) {
        return;
    }
    // Runtime metadata discovery reads a relative descriptor pointer here.
    context.GetAssembler().embedUInt32(0);
    MakePrologue(context);
    if (context.GetFailure()) {
        return;
    }
    if (!SaveParameters(context, *placements)) {
        return;
    }
    if (!SaveHiddenTypeInfo(context, *placements)) {
        return;
    }
    auto& assembler = context.GetAssembler();
    assembler.jmp(context.GetBlockLabel(function.GetBody()->GetEntryBlock()));
    for (const auto* block : function.GetBody()->GetBlocks()) {
        CompileBlock::Compile(context, *block);
        if (context.GetFailure() || context.GetError() != asmjit::kErrorOk) {
            return;
        }
    }
    MakeEpilogue(context);
    context.GetLocalFinalizers().EmitEdges();
}

bool CompileFunction::SaveParameters(FunctionContext& context, const ParameterPlacement& placements) const
{
    // Copy ABI arguments into their allocated stack locations for use by the function body.
    for (const auto* parameter : function.GetParams()) {
        if (!parameter || !parameter->GetType()) {
            context.Fail(JitFailureKind::UnsupportedParameters);
            return false;
        }
        const auto* type = parameter->GetType();
        const bool pointer = type->IsRef() || type->IsCPointer() || type->IsCString();
        const auto width = pointer ? 64U : type->IsBoolean() ? 8U : RegisterUtils::IntegerWidth(*type);
        const bool floating = type->GetTypeKind() == CHIR::Type::TYPE_FLOAT32 ||
            type->GetTypeKind() == CHIR::Type::TYPE_FLOAT64;
        const auto location = placements.locations.find(parameter);
        const auto id = context.GetValueNumeration().GetId(parameter);
        if ((!width && !floating) || location == placements.locations.end() || !id ||
            *id >= context.GetRegisterAllocation().GetLocationCount() ||
            !context.GetRegisterAllocation().GetLocation(*id).IsStack()) {
            context.Fail(JitFailureKind::UnsupportedParameters);
            return false;
        }
        auto& a = context.GetAssembler();
        if (const auto* reg = location->second.AsRegister()) {
            const auto source = RegisterUtils::Get64BitRegister(reg->type);
            if (!source) {
                context.Fail(JitFailureKind::UnsupportedParameters);
                return false;
            }
            a.mov(asmjit::x86::rax, *source);
        } else if (const auto* simd = location->second.AsSimdRegister()) {
            const auto source = RegisterUtils::GetArgumentSimdRegister(simd->index);
            if (!floating || !source || !MemoryUtils::StoreFloatValue(context, parameter, *source, asmjit::x86::rax)) {
                context.Fail(JitFailureKind::UnsupportedParameters);
                return false;
            }
            continue;
        } else if (const auto* stack = location->second.AsIncomingStack()) {
            constexpr size_t BASE_OFFSET = 2 * sizeof(uint64_t);
            if (stack->index > (static_cast<size_t>(std::numeric_limits<int32_t>::max()) - BASE_OFFSET) /
                    sizeof(uint64_t)) {
                context.Fail(JitFailureKind::UnsupportedParameters);
                return false;
            }
            const auto offset = static_cast<int32_t>(BASE_OFFSET + stack->index * sizeof(uint64_t));
            a.mov(asmjit::x86::rax, asmjit::x86::qword_ptr(asmjit::x86::rbp, offset));
        } else {
            context.Fail(JitFailureKind::UnsupportedParameters);
            return false;
        }
        if (width) {
            RegisterUtils::NormalizeInteger(a, asmjit::x86::rax, width, type->IsSignedInteger());
        }
        if (!MemoryUtils::StoreValue(context, parameter, asmjit::x86::rax)) {
            return false;
        }
    }
    return true;
}

bool CompileFunction::SaveHiddenTypeInfo(FunctionContext& context, const ParameterPlacement& placements) const
{
    // Hidden type information follows generic, outer and this type order in the frame.
    size_t hiddenIndex = 0;
    for (const auto& location : placements.genericTypeInfoLocations) {
        if (!SaveHiddenTypeInfoLocation(context, location, hiddenIndex++)) {
            return false;
        }
    }
    if (placements.outerTypeInfoLocation &&
        !SaveHiddenTypeInfoLocation(context, *placements.outerTypeInfoLocation, hiddenIndex++)) {
        return false;
    }
    if (placements.thisTypeInfoLocation &&
        !SaveHiddenTypeInfoLocation(context, *placements.thisTypeInfoLocation, hiddenIndex++)) {
        return false;
    }
    return true;
}

bool CompileFunction::SaveHiddenTypeInfoLocation(FunctionContext& context, const Location& location, size_t index) const
{
    const auto offset = context.GetHiddenTypeInfoOffset(index);
    if (!offset) {
        context.Fail(JitFailureKind::UnsupportedCallingConvention);
        return false;
    }
    auto& a = context.GetAssembler();
    if (const auto* reg = location.AsRegister()) {
        const auto source = RegisterUtils::Get64BitRegister(reg->type);
        if (!source) {
            context.Fail(JitFailureKind::UnsupportedCallingConvention);
            return false;
        }
        a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp, *offset), *source);
    } else if (const auto* stack = location.AsIncomingStack()) {
        constexpr size_t BASE_OFFSET = 2 * sizeof(uint64_t);
        if (stack->index > (static_cast<size_t>(std::numeric_limits<int32_t>::max()) - BASE_OFFSET) /
                sizeof(uint64_t)) {
            context.Fail(JitFailureKind::UnsupportedCallingConvention);
            return false;
        }
        const auto source = static_cast<int32_t>(BASE_OFFSET + stack->index * sizeof(uint64_t));
        a.mov(asmjit::x86::rax, asmjit::x86::qword_ptr(asmjit::x86::rbp, source));
        a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp, *offset), asmjit::x86::rax);
    } else {
        context.Fail(JitFailureKind::UnsupportedCallingConvention);
        return false;
    }
    return true;
}

} // namespace Cangjie::JIT::X86_64
