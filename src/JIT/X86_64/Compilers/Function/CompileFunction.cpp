// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "CompileFunction.h"

#include <utility>
#include <string_view>

#include "../../RegisterAllocation/LivenessAnalysis.h"
#include "../../RegisterAllocation/LivenessInterferenceGraphBuilder.h"
#include "../../RegisterAllocation/TrivialStackRegalloc.h"
#include "../../RegisterAllocation/ValueNumeration.h"
#include "../../Utils/SystemVCallingConvention.h"
#include "../../Utils/MemoryUtils.h"
#include "../../Utils/TypeUtils.h"
#include "../../Utils/RegisterUtils.h"
#include "../../Utils/StackFrameUtils.h"
#include "CompileBlock.h"
#include "../Context/GlobalContext.h"
#include "../Context/FunctionContext.h"
#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Type/Type.h"
#include "cangjie/CHIR/IR/Value/Value.h"
#include "cangjie/Utils/CheckUtils.h"

namespace Cangjie::JIT::X86_64 {

CompileFunction::CompileFunction(GlobalContext& globalContext, const CHIR::Function& function) noexcept
    : globalContext(globalContext), function(function)
{
}


CompilationResult CompileFunction::CompileToAddress() const
{
    globalContext.GetLogger().LogFunctionCompilationBegin(function.GetIdentifier());
    auto result = CompileToAddressImpl();
    globalContext.GetLogger().LogFunctionCompilationEnd(function.GetIdentifier(), static_cast<bool>(result),
        result ? std::string_view{} : std::string_view(result.Error().message));
    return result;
}

void CompileFunction::PrintDebugInfo(const FunctionContext& context) const
{
    globalContext.GetLogger().LogRegisterAllocation(
        function.GetIdentifier(), context.GetValueNumeration(), context.GetRegisterAllocation());
    globalContext.GetLogger().LogLiveness(function.GetIdentifier(), context.GetLiveness(), context.GetValueNumeration());
}

CompilationResult CompileFunction::CompileToAddressImpl() const
{
    if (!function.GetBody() || !function.GetBody()->GetEntryBlock() || !function.GetReturnType() ||
        !function.GetFuncType()) {
        return CompilationResult::Failure({"invalid function"});
    }
    auto valueNumeration = ValueNumerator(function).Run();
    auto liveness = LivenessAnalysis::Analyze(function, valueNumeration);
    auto registerAllocation = TrivialStackRegalloc().Allocate(
        valueNumeration, LivenessInterferenceGraphBuilder().Build(function, valueNumeration, liveness));
    FunctionContext context(globalContext, &function, std::move(valueNumeration), std::move(liveness),
        std::move(registerAllocation));
    if (context.Prepare()) {
        PrintDebugInfo(context);
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
    return Publish(context);
}

CompilationResult CompileFunction::Publish(FunctionContext& context) const
{
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
    globalContext.GetLogger().LogFunctionCreated(function.GetIdentifier(), base + PREFIX_SIZE, codeSize,
        static_cast<size_t>(*frameSize), base + metadataOffset, encoded.descriptor.size());
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
    const auto width = TypeUtils::TypeWidth(*type);
    if (!width) {
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
        TypeUtils::NormalizeInteger(assembler, asmjit::x86::rax, width, type.IsSignedInteger());
    } else if (width == 16) {
        assembler.movzx(asmjit::x86::eax, asmjit::x86::word_ptr(asmjit::x86::rax));
        TypeUtils::NormalizeInteger(assembler, asmjit::x86::rax, width, type.IsSignedInteger());
    } else if (width == 32) {
        assembler.mov(asmjit::x86::eax, asmjit::x86::dword_ptr(asmjit::x86::rax));
        TypeUtils::NormalizeInteger(assembler, asmjit::x86::rax, width, type.IsSignedInteger());
    } else {
        assembler.mov(asmjit::x86::rax, asmjit::x86::qword_ptr(asmjit::x86::rax));
    }
}

void CompileFunction::Compile(FunctionContext& context) const
{
    const auto placements = SystemVCallingConvention::GetNativeParameterPlacement(function);
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
    for (const auto* parameter : function.GetParams()) {
        if (!SaveParameter(context, placements, parameter)) {
            return false;
        }
    }
    return true;
}

bool CompileFunction::SaveParameter(FunctionContext& context, const ParameterPlacement& placements,
    const CHIR::Parameter* parameter) const
{
    const auto* type = parameter->GetType();
    const auto width = TypeUtils::TypeWidth(*type);
    const bool floating = TypeUtils::IsFloating(*type);
    const auto location = placements.locations.at(parameter);
    const auto id = context.GetValueNumeration().GetId(parameter);
    if ((!width && !floating) || !context.GetRegisterAllocation().GetLocation(*id).IsStack()) {
        context.Fail(JitFailureKind::UnsupportedParameters);
        return false;
    }
    if (const auto* simd = location.AsSimdRegister()) {
        const auto source = RegisterUtils::GetArgumentSimdRegister(simd->index);
        if (!floating || !source || !MemoryUtils::StoreFloatValue(context, parameter, *source, asmjit::x86::rax)) {
            context.Fail(JitFailureKind::UnsupportedParameters);
            return false;
        }
        return true;
    }
    auto& assembler = context.GetAssembler();
    if (const auto* reg = location.AsRegister()) {
        const auto source = RegisterUtils::Get64BitRegister(reg->type);
        if (!source) {
            context.Fail(JitFailureKind::UnsupportedParameters);
            return false;
        }
        assembler.mov(asmjit::x86::rax, *source);
    } else if (const auto* stack = location.AsIncomingStack()) {
        const auto offset = StackFrameUtils::GetIncomingArgumentOffset(stack->index);
        if (!offset) {
            context.Fail(JitFailureKind::UnsupportedParameters);
            return false;
        }
        assembler.mov(asmjit::x86::rax, asmjit::x86::qword_ptr(asmjit::x86::rbp, *offset));
    } else {
        context.Fail(JitFailureKind::UnsupportedParameters);
        return false;
    }
    if (!floating && width) {
        TypeUtils::NormalizeInteger(assembler, asmjit::x86::rax, width, type->IsSignedInteger());
    }
    return MemoryUtils::StoreValue(context, parameter, asmjit::x86::rax);
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
        return true;
    }
    const auto* stack = location.AsIncomingStack();
    if (!stack) {
        context.Fail(JitFailureKind::UnsupportedCallingConvention);
        return false;
    }
    const auto source = StackFrameUtils::GetIncomingArgumentOffset(stack->index);
    if (!source) {
        context.Fail(JitFailureKind::UnsupportedCallingConvention);
        return false;
    }
    a.mov(asmjit::x86::rax, asmjit::x86::qword_ptr(asmjit::x86::rbp, *source));
    a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp, *offset), asmjit::x86::rax);
    return true;
}

} // namespace Cangjie::JIT::X86_64
