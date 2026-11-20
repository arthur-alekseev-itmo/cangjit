// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "MemoryUtils.h"

#include <cstring>
#include <limits>

#include "RegisterUtils.h"
#include "StackFrameUtils.h"

#include "../Compilers/Context/FunctionContext.h"
#include "../Compilers/Context/GlobalContext.h"
#include "../Location.h"
#include "cangjie/JIT/JitSymbols.h"
#include "cangjie/CHIR/IR/Type/Type.h"
#include "cangjie/CHIR/IR/Value/LiteralValue.h"
#include "cangjie/CHIR/IR/Value/Value.h"

namespace Cangjie::JIT::X86_64 {
namespace {
struct ResolvedLocation {
    bool isRegister = false;
    asmjit::x86::Gp reg;
    asmjit::x86::Mem memory;
};

bool ResolveLocation(FunctionContext& context, const Location& location, JitFailureKind failure,
    ResolvedLocation& result)
{
    namespace x86 = asmjit::x86;
    if (const auto* reg = location.AsRegister()) {
        auto physical = RegisterUtils::Get64BitRegister(reg->type);
        if (reg->width != OperandWidth::QWORD || !physical) {
            context.Fail(failure);
            return false;
        }
        result.isRegister = true;
        result.reg = *physical;
        return true;
    }
    if (const auto* stack = location.AsStack()) {
        auto offset = StackFrameUtils::GetSlotOffset(stack->index, context.GetRegisterAllocation().GetStackSlotCount());
        if (offset) {
            result.memory = x86::qword_ptr(x86::rbp, *offset);
            return true;
        }
    }
    context.Fail(failure);
    return false;
}

bool HasAggregateValueType(const CHIR::Value* value)
{
    const auto* type = value ? value->GetType() : nullptr;
    return type && (type->IsStruct() || type->IsTuple() || type->IsVArray() || type->IsEnum());
}

bool IsMoveRegister(FunctionContext& context, const asmjit::x86::Gp& reg)
{
    if (!RegisterUtils::IsDataRegister(reg)) {
        context.Fail(JitFailureKind::UnsupportedMoveRegister);
        return false;
    }
    return true;
}
} // namespace

bool MemoryUtils::LoadValue(FunctionContext& context, const CHIR::Value* value, const asmjit::x86::Gp& target)
{
    if (!value) {
        context.Fail(JitFailureKind::MissingOperand);
        return false;
    }
    // Aggregate values cannot be represented by a single scalar stack slot.
    // Until their native layout and GC map are available, do not silently
    // interpret the first word as the whole value.
    if (HasAggregateValueType(value)) {
        context.Fail(JitFailureKind::UnsupportedOperandLocation);
        return false;
    }
    if (!IsMoveRegister(context, target)) {
        return false;
    }
    auto& assembler = context.GetAssembler();
    if (value->IsLiteral()) {
        auto* literal = static_cast<const CHIR::LiteralValue*>(value);
        if (literal->IsBoolLiteral()) {
            assembler.mov(target, static_cast<const CHIR::BoolLiteral*>(literal)->GetVal() ? 1 : 0);
        } else if (literal->IsIntLiteral()) {
            assembler.mov(target, asmjit::imm(static_cast<const CHIR::IntLiteral*>(literal)->GetUnsignedVal()));
        } else if (literal->IsRuneLiteral()) {
            assembler.mov(target, static_cast<uint32_t>(static_cast<const CHIR::RuneLiteral*>(literal)->GetVal()));
        } else if (literal->IsNullLiteral()) {
            assembler.xor_(target.r32(), target.r32());
        } else if (literal->IsFloatLiteral() && value->GetType()) {
            const auto val = static_cast<const CHIR::FloatLiteral*>(literal)->GetVal();
            if (value->GetType()->GetTypeKind() == CHIR::Type::TYPE_FLOAT32) {
                const float converted = static_cast<float>(val);
                uint32_t bits;
                std::memcpy(&bits, &converted, sizeof(bits));
                assembler.mov(target, asmjit::imm(bits));
            } else if (value->GetType()->GetTypeKind() == CHIR::Type::TYPE_FLOAT64) {
                uint64_t bits;
                std::memcpy(&bits, &val, sizeof(bits));
                assembler.mov(target, asmjit::imm(bits));
            } else {
                context.Fail(JitFailureKind::UnsupportedLiteral);
                return false;
            }
        } else {
            context.Fail(JitFailureKind::UnsupportedLiteral);
            return false;
        }
        return context.GetError() == asmjit::kErrorOk;
    }
    if (value->IsGlobalVar()) {
        const auto name = value->IsImportedVar() ? value->GetIdentifierWithoutPrefix() :
            JitGlobalSlotName(value->GetIdentifierWithoutPrefix());
        auto resolved = context.GetGlobalContext().GetRuntimeBindings().Resolve(name);
        const auto* symbol = std::get_if<void*>(&resolved);
        if (!symbol || !*symbol) {
            context.Fail(JitFailureKind::UnresolvedSymbol);
            return false;
        }
        const auto address = value->IsImportedVar() ? *symbol : *static_cast<void* const*>(*symbol);
        if (!address) {
            context.Fail(JitFailureKind::UnresolvedSymbol);
            return false;
        }
        assembler.mov(target, asmjit::imm(reinterpret_cast<uintptr_t>(address)));
        return context.GetError() == asmjit::kErrorOk;
    }
    auto id = context.GetValueNumeration().GetId(value);
    if (!id || *id >= context.GetRegisterAllocation().GetLocationCount()) {
        context.Fail(JitFailureKind::UnallocatedOperand);
        return false;
    }
    ResolvedLocation source;
    if (!ResolveLocation(context, context.GetRegisterAllocation().GetLocation(*id),
        JitFailureKind::UnsupportedOperandLocation, source)) {
        return false;
    }
    if (source.isRegister) {
        if (source.reg.id() != target.id()) {
            assembler.mov(target, source.reg);
        }
    } else {
        assembler.mov(target, source.memory);
    }
    return context.GetError() == asmjit::kErrorOk;
}

bool MemoryUtils::StoreValue(FunctionContext& context, const CHIR::Value* value, const asmjit::x86::Gp& source)
{
    if (HasAggregateValueType(value)) {
        context.Fail(JitFailureKind::UnsupportedResultLocation);
        return false;
    }
    if (!IsMoveRegister(context, source)) {
        return false;
    }
    auto id = context.GetValueNumeration().GetId(value);
    if (!id || *id >= context.GetRegisterAllocation().GetLocationCount()) {
        context.Fail(JitFailureKind::UnallocatedResult);
        return false;
    }
    ResolvedLocation destination;
    if (!ResolveLocation(context, context.GetRegisterAllocation().GetLocation(*id),
        JitFailureKind::UnsupportedResultLocation, destination)) {
        return false;
    }
    auto& assembler = context.GetAssembler();
    if (destination.isRegister) {
        if (destination.reg.id() != source.id()) {
            assembler.mov(destination.reg, source);
        }
    } else {
        assembler.mov(destination.memory, source);
    }
    return context.GetError() == asmjit::kErrorOk;
}

bool MemoryUtils::LoadFloatValue(FunctionContext& context, const CHIR::Value* value,
    const asmjit::x86::Xmm& target, const asmjit::x86::Gp& scratch)
{
    if (!value || !value->GetType()) {
        context.Fail(JitFailureKind::MissingOperand);
        return false;
    }
    const auto kind = value->GetType()->GetTypeKind();
    if (kind != CHIR::Type::TYPE_FLOAT16 && kind != CHIR::Type::TYPE_FLOAT32 &&
        kind != CHIR::Type::TYPE_FLOAT64) {
        context.Fail(JitFailureKind::UnsupportedOperandLocation);
        return false;
    }
    if (!LoadValue(context, value, scratch)) {
        return false;
    }
    auto& assembler = context.GetAssembler();
    if (kind == CHIR::Type::TYPE_FLOAT64) {
        assembler.movq(target, scratch);
    } else {
        assembler.movd(target, scratch.r32());
    }
    return context.GetError() == asmjit::kErrorOk;
}

bool MemoryUtils::StoreFloatValue(FunctionContext& context, const CHIR::Value* value,
    const asmjit::x86::Xmm& source, const asmjit::x86::Gp& scratch)
{
    if (!value || !value->GetType()) {
        context.Fail(JitFailureKind::MissingOperand);
        return false;
    }
    const auto kind = value->GetType()->GetTypeKind();
    if (kind != CHIR::Type::TYPE_FLOAT16 && kind != CHIR::Type::TYPE_FLOAT32 &&
        kind != CHIR::Type::TYPE_FLOAT64) {
        context.Fail(JitFailureKind::UnsupportedResultLocation);
        return false;
    }
    if (!IsMoveRegister(context, scratch)) {
        return false;
    }
    auto& assembler = context.GetAssembler();
    if (kind == CHIR::Type::TYPE_FLOAT64) {
        assembler.movq(scratch, source);
    } else {
        assembler.movd(scratch.r32(), source);
    }
    return StoreValue(context, value, scratch);
}

void MemoryUtils::ReserveStack(FunctionContext& context, uint32_t size)
{
    if (size != 0) {
        context.GetAssembler().sub(asmjit::x86::rsp, size);
    }
}

void MemoryUtils::ReleaseStack(FunctionContext& context, uint32_t size)
{
    if (size != 0) {
        context.GetAssembler().add(asmjit::x86::rsp, size);
    }
}

bool MemoryUtils::StoreOutgoingArgument(
    FunctionContext& context, const CHIR::Value* value, size_t index, uint32_t stackSize)
{
    constexpr size_t SLOT_SIZE = sizeof(uint64_t);
    if (index >= stackSize / SLOT_SIZE || index > static_cast<size_t>(std::numeric_limits<int32_t>::max()) / SLOT_SIZE) {
        context.Fail(JitFailureKind::UnsupportedCallingConvention);
        return false;
    }
    if (!LoadValue(context, value, asmjit::x86::rax)) {
        return false;
    }
    context.GetAssembler().mov(
        asmjit::x86::qword_ptr(asmjit::x86::rsp, static_cast<int32_t>(index * SLOT_SIZE)), asmjit::x86::rax);
    return context.GetError() == asmjit::kErrorOk;
}

bool MemoryUtils::ZeroStackValueAndLoadAddress(
    FunctionContext& context, const CHIR::Value* value, const asmjit::x86::Gp& target)
{
    if (!IsMoveRegister(context, target)) {
        return false;
    }
    const auto& allocation = context.GetRegisterAllocation();
    const auto id = context.GetValueNumeration().GetId(value);
    const auto* location = id && *id < allocation.GetLocationCount()
        ? allocation.GetLocation(*id).AsStack() : nullptr;
    const auto offset = location
        ? StackFrameUtils::GetSlotOffset(location->index, allocation.GetStackSlotCount()) : std::nullopt;
    if (!offset) {
        context.Fail(JitFailureKind::UnallocatedResult);
        return false;
    }
    auto& assembler = context.GetAssembler();
    const auto memory = asmjit::x86::qword_ptr(asmjit::x86::rbp, *offset);
    assembler.mov(memory, 0);
    assembler.lea(target, memory);
    return context.GetError() == asmjit::kErrorOk;
}

bool MemoryUtils::Move(FunctionContext& context, const Location& destination, const Location& source,
    const asmjit::x86::Gp& scratch)
{
    ResolvedLocation from;
    ResolvedLocation to;
    if (!ResolveLocation(context, source, JitFailureKind::UnsupportedMoveSource, from) ||
        !ResolveLocation(context, destination, JitFailureKind::UnsupportedMoveDestination, to)) {
        return false;
    }
    auto& assembler = context.GetAssembler();
    if (from.isRegister && to.isRegister) {
        if (from.reg.id() != to.reg.id()) {
            assembler.mov(to.reg, from.reg);
        }
    } else if (from.isRegister) {
        assembler.mov(to.memory, from.reg);
    } else if (to.isRegister) {
        assembler.mov(to.reg, from.memory);
    } else if (source.AsStack()->index != destination.AsStack()->index) {
        if (!RegisterUtils::IsDataRegister(scratch)) {
            context.Fail(JitFailureKind::InvalidMoveScratch);
            return false;
        }
        assembler.mov(scratch, from.memory);
        assembler.mov(to.memory, scratch);
    }
    return context.GetError() == asmjit::kErrorOk;
}
} // namespace Cangjie::JIT::X86_64
