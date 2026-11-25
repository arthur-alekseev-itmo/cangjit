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
#include "TypeUtils.h"

#include "../Compilers/Context/FunctionContext.h"
#include "../Compilers/Context/GlobalContext.h"
#include "../Location.h"
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
    if (const auto* reg = location.AsGpRegister()) {
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
bool IsSameLocation(const Location& left, const Location& right)
{
    if (const auto* reg = left.AsGpRegister()) {
        const auto* other = right.AsGpRegister();
        return other && reg->type == other->type;
    }
    if (const auto* simd = left.AsSimdRegister()) {
        const auto* other = right.AsSimdRegister();
        return other && simd->index == other->index;
    }
    if (const auto* stack = left.AsStack()) {
        const auto* other = right.AsStack();
        return other && stack->index == other->index;
    }
    const auto* stack = left.AsIncomingStack();
    const auto* other = right.AsIncomingStack();
    return other && stack->index == other->index;
}

asmjit::x86::Mem GetMoveMemory(FunctionContext& context, const Location& location)
{
    namespace x86 = asmjit::x86;
    if (const auto* stack = location.AsStack()) {
        const auto offset = StackFrameUtils::GetSlotOffset(
            stack->index, context.GetRegisterAllocation().GetStackSlotCount());
        return x86::qword_ptr(x86::rbp, *offset);
    }
    const auto* stack = location.AsIncomingStack();
    const auto offset = StackFrameUtils::GetIncomingArgumentOffset(stack->index);
    return x86::qword_ptr(x86::rbp, *offset);
}

void LoadMoveSource(FunctionContext& context, const Location& source, const asmjit::x86::Gp& target)
{
    auto& assembler = context.GetAssembler();
    if (const auto* reg = source.AsGpRegister()) {
        const auto physical = RegisterUtils::Get64BitRegister(reg->type);
        if (physical->id() != target.id()) {
            assembler.mov(target, *physical);
        }
    } else if (const auto* simd = source.AsSimdRegister()) {
        const auto physical = RegisterUtils::GetArgumentSimdRegister(simd->index);
        if (simd->width == OperandWidth::QWORD) {
            assembler.movq(target, *physical);
        } else {
            assembler.movd(target.r32(), *physical);
        }
    } else {
        assembler.mov(target, GetMoveMemory(context, source));
    }
}

void StoreMoveDestination(FunctionContext& context, const Location& destination,
    const asmjit::x86::Gp& source)
{
    auto& assembler = context.GetAssembler();
    if (const auto* reg = destination.AsGpRegister()) {
        const auto physical = RegisterUtils::Get64BitRegister(reg->type);
        if (physical->id() != source.id()) {
            assembler.mov(*physical, source);
        }
    } else if (const auto* simd = destination.AsSimdRegister()) {
        const auto physical = RegisterUtils::GetArgumentSimdRegister(simd->index);
        if (simd->width == OperandWidth::QWORD) {
            assembler.movq(*physical, source);
        } else {
            assembler.movd(*physical, source.r32());
        }
    } else {
        assembler.mov(GetMoveMemory(context, destination), source);
    }
}

unsigned MoveWidth(const CHIR::Type& type, const Location& source)
{
    const auto width = TypeUtils::TypeWidth(type);
    if (width) {
        return width;
    }
    if (const auto* reg = source.AsGpRegister()) {
        return 8U << static_cast<unsigned>(reg->width);
    }
    if (const auto* simd = source.AsSimdRegister()) {
        return 8U << static_cast<unsigned>(simd->width);
    }
    return 0;
}

bool IsFloatingValue(const CHIR::Type& type)
{
    return type.GetTypeKind() == CHIR::Type::TYPE_FLOAT16 || TypeUtils::IsFloating(type);
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
    if (value->IsLiteral()) {
        return LoadLiteral(context, static_cast<const CHIR::LiteralValue&>(*value), target);
    }
    if (value->IsGlobalVar()) {
        return LoadGlobal(context, *value, target);
    }
    auto& assembler = context.GetAssembler();
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

bool MemoryUtils::LoadLiteral(
    FunctionContext& context, const CHIR::LiteralValue& literal, const asmjit::x86::Gp& target)
{
    auto& assembler = context.GetAssembler();
    if (literal.IsBoolLiteral()) {
        assembler.mov(target, static_cast<const CHIR::BoolLiteral&>(literal).GetVal() ? 1 : 0);
    } else if (literal.IsIntLiteral()) {
        assembler.mov(target, asmjit::imm(static_cast<const CHIR::IntLiteral&>(literal).GetUnsignedVal()));
    } else if (literal.IsRuneLiteral()) {
        assembler.mov(target, static_cast<uint32_t>(static_cast<const CHIR::RuneLiteral&>(literal).GetVal()));
    } else if (literal.IsNullLiteral()) {
        assembler.xor_(target.r32(), target.r32());
    } else if (literal.IsFloatLiteral()) {
        return LoadFloatLiteral(context, literal, target);
    } else {
        context.Fail(JitFailureKind::UnsupportedLiteral);
        return false;
    }
    return context.GetError() == asmjit::kErrorOk;
}

bool MemoryUtils::LoadFloatLiteral(
    FunctionContext& context, const CHIR::LiteralValue& literal, const asmjit::x86::Gp& target)
{
    const auto* type = literal.GetType();
    if (!type) {
        context.Fail(JitFailureKind::UnsupportedLiteral);
        return false;
    }
    const auto value = static_cast<const CHIR::FloatLiteral&>(literal).GetVal();
    auto& assembler = context.GetAssembler();
    if (type->GetTypeKind() == CHIR::Type::TYPE_FLOAT32) {
        const float converted = static_cast<float>(value);
        uint32_t bits;
        std::memcpy(&bits, &converted, sizeof(bits));
        assembler.mov(target, asmjit::imm(bits));
    } else if (type->GetTypeKind() == CHIR::Type::TYPE_FLOAT64) {
        uint64_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        assembler.mov(target, asmjit::imm(bits));
    } else {
        context.Fail(JitFailureKind::UnsupportedLiteral);
        return false;
    }
    return context.GetError() == asmjit::kErrorOk;
}

bool MemoryUtils::LoadGlobal(FunctionContext& context, const CHIR::Value& value, const asmjit::x86::Gp& target)
{
    if (!value.IsImportedVar()) {
        context.Fail(JitFailureKind::UnsupportedPackageGlobal);
        return false;
    }
    auto& bindings = context.GetGlobalContext().GetRuntimeBindings();
    if (!bindings.EnsurePackage(static_cast<const CHIR::GlobalVar&>(value).GetPackageName())) {
        context.Fail(JitFailureKind::UnresolvedSymbol);
        return false;
    }
    auto resolved = bindings.Resolve(value.GetIdentifierWithoutPrefix());
    if (!resolved) {
        context.Fail(JitFailureKind::UnresolvedSymbol);
        return false;
    }
    context.GetAssembler().mov(target, asmjit::imm(reinterpret_cast<uintptr_t>(resolved.Value())));
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

bool MemoryUtils::PlaceTypeInfo(
    FunctionContext& context, const CHIR::Type* type, const Location& location, uint32_t stackSize)
{
    if (!type) {
        context.Fail(JitFailureKind::UnsupportedCallingConvention);
        return false;
    }
    if (const auto* reg = location.AsGpRegister()) {
        const auto target = RegisterUtils::Get64BitRegister(reg->type);
        if (!target) {
            context.Fail(JitFailureKind::UnsupportedCallingConvention);
            return false;
        }
        return context.GetTypeInfoEmitter().LoadTypeInfo(*type, *target);
    }
    const auto* stack = location.AsIncomingStack();
    if (!stack || stack->index >= stackSize / sizeof(uint64_t) ||
        stack->index > static_cast<size_t>(std::numeric_limits<int32_t>::max()) / sizeof(uint64_t)) {
        context.Fail(JitFailureKind::UnsupportedCallingConvention);
        return false;
    }
    if (!context.GetTypeInfoEmitter().LoadTypeInfo(*type, asmjit::x86::rax)) {
        return false;
    }
    context.GetAssembler().mov(
        asmjit::x86::qword_ptr(asmjit::x86::rsp, static_cast<int32_t>(stack->index * sizeof(uint64_t))),
        asmjit::x86::rax);
    return true;
}

std::optional<uint32_t> MemoryUtils::StageTypeInfos(
    FunctionContext& context, const std::vector<const CHIR::Type*>& types)
{
    if (types.size() > (static_cast<size_t>(std::numeric_limits<int32_t>::max()) - 15) / sizeof(uint64_t)) {
        context.Fail(JitFailureKind::UnsupportedCallingConvention);
        return std::nullopt;
    }
    const auto size = static_cast<uint32_t>((types.size() * sizeof(uint64_t) + 15) & ~size_t{15});
    ReserveStack(context, size);
    for (size_t i = 0; i < types.size(); ++i) {
        if (!types[i] || !context.GetTypeInfoEmitter().LoadTypeInfo(*types[i], asmjit::x86::rax)) {
            context.Fail(JitFailureKind::UnsupportedCall);
            return std::nullopt;
        }
        context.GetAssembler().mov(
            asmjit::x86::qword_ptr(asmjit::x86::rsp, static_cast<int32_t>(i * sizeof(uint64_t))),
            asmjit::x86::rax);
    }
    return size;
}

bool MemoryUtils::PlaceStagedTypeInfo(
    FunctionContext& context, const Location& location, uint32_t stackSize, int32_t scratchOffset)
{
    auto& assembler = context.GetAssembler();
    if (const auto* reg = location.AsGpRegister()) {
        const auto target = RegisterUtils::Get64BitRegister(reg->type);
        if (!target) {
            context.Fail(JitFailureKind::UnsupportedCallingConvention);
            return false;
        }
        assembler.mov(*target, asmjit::x86::qword_ptr(asmjit::x86::rsp, scratchOffset));
        return true;
    }
    const auto* stack = location.AsIncomingStack();
    if (!stack || stack->index >= stackSize / sizeof(uint64_t) ||
        stack->index > static_cast<size_t>(std::numeric_limits<int32_t>::max()) / sizeof(uint64_t)) {
        context.Fail(JitFailureKind::UnsupportedCallingConvention);
        return false;
    }
    assembler.mov(asmjit::x86::rax, asmjit::x86::qword_ptr(asmjit::x86::rsp, scratchOffset));
    assembler.mov(
        asmjit::x86::qword_ptr(asmjit::x86::rsp, static_cast<int32_t>(stack->index * sizeof(uint64_t))),
        asmjit::x86::rax);
    return true;
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
    const CHIR::Type& type, const asmjit::x86::Gp& scratch)
{
    auto& assembler = context.GetAssembler();
    const auto width = MoveWidth(type, source);
    const bool floating = IsFloatingValue(type);
    if (IsSameLocation(destination, source) && (floating || !width || width == 64)) {
        return context.GetError() == asmjit::kErrorOk;
    }
    if (const auto* from = source.AsGpRegister()) {
        if (const auto* to = destination.AsGpRegister()) {
            const auto sourceRegister = RegisterUtils::Get64BitRegister(from->type);
            const auto destinationRegister = RegisterUtils::Get64BitRegister(to->type);
            if (sourceRegister->id() != destinationRegister->id()) {
                assembler.mov(*destinationRegister, *sourceRegister);
            }
            if (!floating && width && width != 64) {
                TypeUtils::NormalizeInteger(assembler, *destinationRegister, width, type.IsSignedInteger());
            }
            return context.GetError() == asmjit::kErrorOk;
        }
    }
    if (const auto* from = source.AsSimdRegister()) {
        if (const auto* to = destination.AsSimdRegister()) {
            const auto sourceRegister = RegisterUtils::GetArgumentSimdRegister(from->index);
            const auto destinationRegister = RegisterUtils::GetArgumentSimdRegister(to->index);
            if (sourceRegister->id() != destinationRegister->id()) {
                if (width == 64) {
                    assembler.movsd(*destinationRegister, *sourceRegister);
                } else {
                    assembler.movss(*destinationRegister, *sourceRegister);
                }
            }
            return context.GetError() == asmjit::kErrorOk;
        }
    }
    LoadMoveSource(context, source, scratch);
    if (!floating && width && width != 64) {
        TypeUtils::NormalizeInteger(assembler, scratch, width, type.IsSignedInteger());
    }
    StoreMoveDestination(context, destination, scratch);
    return context.GetError() == asmjit::kErrorOk;
}
} // namespace Cangjie::JIT::X86_64
