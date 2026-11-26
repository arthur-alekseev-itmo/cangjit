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
        return CompilationResult::Failure({std::move(message)});
    }
    if (context.GetError() != asmjit::kErrorOk) {
        return CompilationResult::Failure({asmjit::DebugUtils::errorAsString(context.GetError())});
    }
    return Publish(context);
}

CompilationResult CompileFunction::Publish(FunctionContext& context) const
{
    uint32_t codeSize = 0;
    uint32_t frameSize = 0;
    size_t metadataOffset = 0;
    size_t metadataSize = 0;
    CompilationError error;
    if (!GetCodeSize(context, codeSize, error) || !GetFrameSize(context, frameSize, error) ||
        !PrepareMetadata(context, codeSize, frameSize, metadataOffset, metadataSize, error)) {
        return CompilationResult::Failure(std::move(error));
    }
    auto allocation = AllocateCode(context);
    if (!allocation) {
        return allocation;
    }
    const auto base = allocation.Value();
    size_t publishedMetadataSize = 0;
    if (!WriteMetadata(context, base, codeSize, metadataOffset, metadataSize, publishedMetadataSize, error)) {
        globalContext.GetRuntime().release(reinterpret_cast<void*>(base));
        return CompilationResult::Failure(std::move(error));
    }
    if (!RegisterLazyCallSites(context, base)) {
        globalContext.GetRuntime().release(reinterpret_cast<void*>(base));
        return CompilationResult::Failure({"failed to register lazy call sites"});
    }
    globalContext.GetLogger().LogFunctionCreated(function.GetIdentifier(), base + sizeof(uint32_t), codeSize,
        frameSize, base + metadataOffset, publishedMetadataSize);
    return CompilationResult::Success(base + sizeof(uint32_t));
}

bool CompileFunction::GetCodeSize(FunctionContext& context, uint32_t& codeSize, CompilationError& error) const
{
    constexpr size_t PREFIX_SIZE = sizeof(uint32_t);
    const auto offset = context.GetAssembler().offset();
    if (offset <= PREFIX_SIZE || offset - PREFIX_SIZE > UINT32_MAX) {
        error = {"invalid JIT code size"};
        return false;
    }
    codeSize = static_cast<uint32_t>(offset - PREFIX_SIZE);
    return true;
}

bool CompileFunction::GetFrameSize(FunctionContext& context, uint32_t& frameSize, CompilationError& error) const
{
    const auto slots = context.GetRegisterAllocation().GetStackSlotCount();
    const auto size = StackFrameUtils::GetFrameSize(slots + 1);
    if (!size) {
        error = {"JIT stack frame is too large"};
        return false;
    }
    frameSize = *size;
    return true;
}

bool CompileFunction::PrepareMetadata(FunctionContext& context, uint32_t codeSize, uint32_t frameSize,
    size_t& metadataOffset, size_t& metadataSize, CompilationError& error) const
{
    auto& assembler = context.GetAssembler();
    context.GetMetadata().RecordFrame(frameSize,
        {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX});
    constexpr uintptr_t PROVISIONAL_CODE = 0x10000000;
    constexpr uintptr_t PROVISIONAL_METADATA = 0x20000000;
    auto provisional = context.GetMetadata().Serialize(PROVISIONAL_METADATA, PROVISIONAL_CODE, codeSize);
    if (!provisional) {
        error = {provisional.Error()};
        return false;
    }
    assembler.align(asmjit::AlignMode::kData, 4);
    metadataOffset = assembler.offset();
    const auto& descriptor = provisional.Value().descriptor;
    metadataSize = descriptor.size();
    assembler.embed(descriptor.data(), descriptor.size());
    if (context.GetError() != asmjit::kErrorOk) {
        error = {asmjit::DebugUtils::errorAsString(context.GetError())};
        return false;
    }
    return true;
}

CompilationResult CompileFunction::AllocateCode(FunctionContext& context) const
{
    void* address = nullptr;
    const auto error = globalContext.GetRuntime().add(&address, &context.GetCode());
    if (error != asmjit::kErrorOk) {
        return CompilationResult::Failure({asmjit::DebugUtils::errorAsString(error)});
    }
    return CompilationResult::Success(reinterpret_cast<CodeAddress>(address));
}

bool CompileFunction::WriteMetadata(FunctionContext& context, CodeAddress base, uint32_t codeSize,
    size_t metadataOffset, size_t metadataSize, size_t& publishedMetadataSize, CompilationError& error) const
{
    auto metadata = context.GetMetadata().Serialize(base + metadataOffset, base + sizeof(uint32_t), codeSize);
    if (!metadata) {
        error = {metadata.Error()};
        return false;
    }
    const auto& encoded = metadata.Value();
    publishedMetadataSize = encoded.descriptor.size();
    asmjit::JitAllocator::Span span;
    auto* allocator = globalContext.GetRuntime().allocator();
    if (publishedMetadataSize != metadataSize || allocator->query(span, reinterpret_cast<void*>(base)) != asmjit::kErrorOk ||
        allocator->write(span, metadataOffset, encoded.descriptor.data(), publishedMetadataSize) != asmjit::kErrorOk ||
        allocator->write(span, 0, encoded.prefix.data(), encoded.prefix.size()) != asmjit::kErrorOk) {
        error = {"failed to publish JIT metadata"};
        return false;
    }
    return true;
}

bool CompileFunction::RegisterLazyCallSites(FunctionContext& context, CodeAddress base) const
{
    return globalContext.GetLazyCallSiteRegistry().Register(
        base, context.GetCode().codeSize(), context.GetLazyCallSites());
}

/// Function prologue:
///   push rbp
///   mov rbp rsp
///   call L1
/// L1:
///   sub rsp <stack_space>
void CompileFunction::MakePrologue(FunctionContext& context) const
{
    const auto slots = context.GetRegisterAllocation().GetStackSlotCount();
    const auto size = StackFrameUtils::GetFrameSize(slots + 1);
    if (!size) {
        context.Fail(JitFailureKind::StackFrameTooLarge);
        return;
    }
    auto& assembler = context.GetAssembler();
    const auto marker = assembler.newLabel();
    assembler.push(asmjit::x86::rbp);
    assembler.mov(asmjit::x86::rbp, asmjit::x86::rsp);
    assembler.call(marker);
    assembler.bind(marker);
    if (*size > sizeof(uint64_t)) {
        assembler.sub(asmjit::x86::rsp, *size - sizeof(uint64_t));
    }
}

/// Function epilogue:
/// L<epiloge_label>:
///   mov rsp rbp
///   pop rbp
///   ret
void CompileFunction::MakeEpilogue(FunctionContext& context, const ParameterPlacement& placement) const
{
    auto& assembler = context.GetAssembler();
    assembler.bind(context.GetEpilogueLabel());
    if (!SaveReturnValue(context, placement)) {
        return;
    }
    assembler.mov(asmjit::x86::rsp, asmjit::x86::rbp);
    assembler.pop(asmjit::x86::rbp);
    assembler.ret();
}

bool CompileFunction::SaveReturnValue(FunctionContext& context, const ParameterPlacement& placement) const
{
    const auto* type = function.GetReturnType();
    if (type->IsUnit() || type->IsVoid() || type->IsNothing()) {
        return true;
    }
    if (placement.indirectReturn || !placement.returnLocation) {
        context.Fail(JitFailureKind::UnsupportedReturnType);
        return false;
    }
    const auto* result = function.GetReturnValue();
    const auto id = context.GetValueNumeration().GetId(result);
    const auto slot = id ? context.GetRegisterAllocation().GetAllocationSlot(*id) : std::nullopt;
    if (!slot || !TypeUtils::TypeWidth(*type) || type->GetTypeKind() == CHIR::Type::TYPE_FLOAT16) {
        context.Fail(JitFailureKind::UnsupportedReturnType);
        return false;
    }
    return MemoryUtils::Move(context, *placement.returnLocation, Location::Stack(*slot), *type, asmjit::x86::rax);
}


void CompileFunction::Compile(FunctionContext& context) const
{
    const auto placements = SystemVCallingConvention::GetNativeParameterPlacement(function);
    if (!placements) {
        context.Fail(JitFailureKind::UnsupportedCallingConvention);
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
    MakeEpilogue(context, *placements);
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
    const auto id = context.GetValueNumeration().GetId(parameter);
    const auto& destination = context.GetRegisterAllocation().GetLocation(*id);
    const auto& source = placements.locations.at(parameter);
    return MemoryUtils::Move(context, destination, source, *type, asmjit::x86::rax);
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
    if (const auto* reg = location.AsGpRegister()) {
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
