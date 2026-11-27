// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "CompileFunction.h"

#include <utility>

#include "../../RegisterAllocation/LivenessAnalysis.h"
#include "../../RegisterAllocation/LivenessInterferenceGraphBuilder.h"
#include "../../RegisterAllocation/TrivialStackRegalloc.h"
#include "../../RegisterAllocation/ValueNumeration.h"
#include "../../Utils/SystemVCallingConvention.h"
#include "../../Utils/MemoryUtils.h"
#include "../../Utils/TypeUtils.h"
#include "../../Utils/RegisterUtils.h"
#include "../../Utils/StackFrameUtils.h"
#include "../../JitAssert.h"
#include "../../../Runtime/RuntimeError.h"
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


CodeAddress CompileFunction::CompileToAddress() const
{
    globalContext.GetLogger().LogFunctionCompilationBegin(function.GetIdentifier());
    const auto address = CompileToAddressImpl();
    globalContext.GetLogger().LogFunctionCompilationEnd(function.GetIdentifier(), true, {});
    return address;
}

void CompileFunction::PrintDebugInfo(const FunctionContext& context) const
{
    globalContext.GetLogger().LogRegisterAllocation(
        function.GetIdentifier(), context.GetValueNumeration(), context.GetRegisterAllocation());
    globalContext.GetLogger().LogLiveness(function.GetIdentifier(), context.GetLiveness(), context.GetValueNumeration());
}

CodeAddress CompileFunction::CompileToAddressImpl() const
{
    // A function that passed frontend compilation always has these.
    JIT_ASSERT(function.GetBody() && function.GetBody()->GetEntryBlock() && function.GetReturnType() &&
        function.GetFuncType(), "compiled function has body, entry block, return and function type");
    auto valueNumeration = ValueNumerator(function).Run();
    auto liveness = LivenessAnalysis::Analyze(function, valueNumeration);
    auto registerAllocation = TrivialStackRegalloc().Allocate(
        valueNumeration, LivenessInterferenceGraphBuilder().Build(function, valueNumeration, liveness));
    FunctionContext context(globalContext, &function, std::move(valueNumeration), std::move(liveness),
        std::move(registerAllocation));
    context.Prepare();
    PrintDebugInfo(context);
    Compile(context);
    const auto error = context.GetError();
    if (error == asmjit::kErrorOutOfMemory) {
        throw RuntimeError{RuntimeErrorKind::OutOfMemory, 0, "emitting " + function.GetIdentifier()};
    }
    JIT_ASSERT(error == asmjit::kErrorOk, asmjit::DebugUtils::errorAsString(error));
    return Publish(context);
}

CodeAddress CompileFunction::Publish(FunctionContext& context) const
{
    constexpr size_t PREFIX_SIZE = sizeof(uint32_t);
    const auto offset = context.GetAssembler().offset();
    // The four-byte metadata prefix and the prologue are always emitted.
    JIT_ASSERT(offset > PREFIX_SIZE && offset - PREFIX_SIZE <= UINT32_MAX, "emitted code has a valid size");
    const auto codeSize = static_cast<uint32_t>(offset - PREFIX_SIZE);
    const auto frameSize = StackFrameUtils::GetFrameSize(context.GetRegisterAllocation().GetStackSlotCount() + 1);
    auto& assembler = context.GetAssembler();
    context.GetMetadata().RecordFrame(frameSize,
        {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX});
    constexpr uintptr_t PROVISIONAL_CODE = 0x10000000;
    constexpr uintptr_t PROVISIONAL_METADATA = 0x20000000;
    auto provisional = context.GetMetadata().Serialize(PROVISIONAL_METADATA, PROVISIONAL_CODE, codeSize);
    assembler.align(asmjit::AlignMode::kData, 4);
    const auto metadataOffset = assembler.offset();
    const auto& descriptor = provisional.descriptor;
    assembler.embed(descriptor.data(), descriptor.size());
    JIT_ASSERT(context.GetError() == asmjit::kErrorOk, "metadata embedding succeeded");
    void* address = nullptr;
    const auto allocationError = globalContext.GetRuntime().add(&address, &context.GetCode());
    if (allocationError == asmjit::kErrorOutOfMemory) {
        throw RuntimeError{RuntimeErrorKind::OutOfMemory, 0, "allocating executable code"};
    }
    JIT_ASSERT(allocationError == asmjit::kErrorOk, "allocating executable code");
    const auto base = reinterpret_cast<CodeAddress>(address);
    auto metadata = context.GetMetadata().Serialize(base + metadataOffset, base + PREFIX_SIZE, codeSize);
    JIT_ASSERT(metadata.descriptor.size() == descriptor.size(), "metadata size is address independent");
    asmjit::JitAllocator::Span span;
    auto allocator = globalContext.GetRuntime().allocator();
    const auto writeError = allocator->query(span, reinterpret_cast<void*>(base)) != asmjit::kErrorOk ||
        allocator->write(span, metadataOffset, metadata.descriptor.data(), metadata.descriptor.size()) ||
        allocator->write(span, 0, metadata.prefix.data(), metadata.prefix.size());
    if (writeError) {
        globalContext.GetRuntime().release(reinterpret_cast<void*>(base));
        JIT_FAIL("failed to publish JIT metadata");
    }
    globalContext.GetLazyCallSiteRegistry().Register(
        base, context.GetCode().codeSize(), context.GetLazyCallSites());
    globalContext.GetLogger().LogFunctionCreated(function.GetIdentifier(), base + PREFIX_SIZE, codeSize,
        frameSize, base + metadataOffset, metadata.descriptor.size());
    return base + PREFIX_SIZE;
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
    auto& assembler = context.GetAssembler();
    const auto marker = assembler.newLabel();
    assembler.push(asmjit::x86::rbp);
    assembler.mov(asmjit::x86::rbp, asmjit::x86::rsp);
    assembler.call(marker);
    assembler.bind(marker);
    if (size > sizeof(uint64_t)) {
        assembler.sub(asmjit::x86::rsp, size - sizeof(uint64_t));
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
    SaveReturnValue(context, placement);
    assembler.mov(asmjit::x86::rsp, asmjit::x86::rbp);
    assembler.pop(asmjit::x86::rbp);
    assembler.ret();
}

void CompileFunction::SaveReturnValue(FunctionContext& context, const ParameterPlacement& placement) const
{
    auto type = function.GetReturnType();
    if (type->IsUnit() || type->IsVoid() || type->IsNothing()) {
        return;
    }
    // SupportedSignature rejects indirect returns for non-Unit results before
    // compilation, so the ABI placement always carries a return location here.
    JIT_ASSERT(!placement.indirectReturn && placement.returnLocation.has_value(),
        "function return uses the supported ABI placement");
    auto result = function.GetReturnValue();
    JIT_ASSERT(result != nullptr, "function has a return value");
    const auto id = context.GetValueNumeration().RequireId(result);
    const auto slot = context.GetRegisterAllocation().GetAllocationSlot(id);
    JIT_ASSERT(slot && TypeUtils::TypeWidth(*type) && type->GetTypeKind() != CHIR::Type::TYPE_FLOAT16,
        "function return value has a supported scalar stack slot");
    MemoryUtils::Move(context, *placement.returnLocation, Location::Stack(*slot), *type, asmjit::x86::rax);
}


void CompileFunction::Compile(FunctionContext& context) const
{
    const auto placements = SystemVCallingConvention::GetNativeParameterPlacement(function);
    // Runtime metadata discovery reads a relative descriptor pointer here.
    context.GetAssembler().embedUInt32(0);
    MakePrologue(context);
    SaveParameters(context, placements);
    SaveHiddenTypeInfo(context, placements);
    auto& assembler = context.GetAssembler();
    assembler.jmp(context.GetBlockLabel(function.GetBody()->GetEntryBlock()));
    for (auto block : function.GetBody()->GetBlocks()) {
        CompileBlock::Compile(context, *block);
    }
    MakeEpilogue(context, placements);
}

void CompileFunction::SaveParameters(FunctionContext& context, const ParameterPlacement& placements) const
{
    for (auto parameter : function.GetParams()) {
        SaveParameter(context, placements, parameter);
    }
}

void CompileFunction::SaveParameter(FunctionContext& context, const ParameterPlacement& placements,
    const CHIR::Parameter* parameter) const
{
    auto type = parameter->GetType();
    const auto id = context.GetValueNumeration().RequireId(parameter);
    const auto& destination = context.GetRegisterAllocation().GetLocation(id);
    const auto& source = placements.locations.at(parameter);
    MemoryUtils::Move(context, destination, source, *type, asmjit::x86::rax);
}

void CompileFunction::SaveHiddenTypeInfo(FunctionContext& context, const ParameterPlacement& placements) const
{
    // Hidden type information follows generic, outer and this type order in the frame.
    size_t hiddenIndex = 0;
    for (const auto& location : placements.genericTypeInfoLocations) {
        SaveHiddenTypeInfoLocation(context, location, hiddenIndex++);
    }
    if (placements.outerTypeInfoLocation) {
        SaveHiddenTypeInfoLocation(context, *placements.outerTypeInfoLocation, hiddenIndex++);
    }
    if (placements.thisTypeInfoLocation) {
        SaveHiddenTypeInfoLocation(context, *placements.thisTypeInfoLocation, hiddenIndex++);
    }
}

void CompileFunction::SaveHiddenTypeInfoLocation(FunctionContext& context, const Location& location, size_t index) const
{
    const auto offset = context.GetHiddenTypeInfoOffset(index);
    auto& a = context.GetAssembler();
    if (auto reg = location.AsGpRegister()) {
        const auto source = RegisterUtils::Get64BitRegister(reg->type);
        a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp, offset), source);
        return;
    }
    auto stack = location.AsIncomingStack();
    JIT_ASSERT(stack != nullptr, "hidden type information arrives in a register or on the stack");
    const auto source = StackFrameUtils::GetIncomingArgumentOffset(stack->index);
    a.mov(asmjit::x86::rax, asmjit::x86::qword_ptr(asmjit::x86::rbp, source));
    a.mov(asmjit::x86::qword_ptr(asmjit::x86::rbp, offset), asmjit::x86::rax);
}

} // namespace Cangjie::JIT::X86_64
