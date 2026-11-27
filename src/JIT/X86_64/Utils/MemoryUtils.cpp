// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "MemoryUtils.h"

#include "RegisterUtils.h"
#include "StackFrameUtils.h"
#include "TypeUtils.h"

#include "../Compilers/Context/FunctionContext.h"
#include "../Location.h"
#include "cangjie/CHIR/IR/Type/Type.h"

namespace Cangjie::JIT::X86_64 {
void MemoryUtils::Move(FunctionContext& context, const Location& destination, const Location& source,
    const CHIR::Type& type, const asmjit::x86::Gp& scratch)
{
    namespace x86 = asmjit::x86;
    auto& assembler = context.GetAssembler();
    size_t width = TypeUtils::TypeWidth(type);
    if (!width) {
        if (auto reg = source.AsGpRegister()) {
            width = 8U << static_cast<unsigned>(reg->width);
        } else if (auto simd = source.AsSimdRegister()) {
            width = 8U << static_cast<unsigned>(simd->width);
        }
    }
    const bool floating = TypeUtils::IsFloatingRepresentation(type);
    if (auto to = destination.AsStack()) {
        auto from = source.AsStack();
        if (from && from->index == to->index && from->base == to->base &&
            (floating || !width || width == 64)) {
            return;
        }
    }
    const auto getMemory = [&](const Location& location) {
        if (auto stack = location.AsStack()) {
            if (stack->base == Location::StackBase::Rsp) {
                return x86::qword_ptr(x86::rsp, static_cast<int32_t>(stack->index * sizeof(uint64_t)));
            }
            const auto offset = StackFrameUtils::GetSlotOffset(
                stack->index, context.GetRegisterAllocation().GetStackSlotCount());
            return x86::qword_ptr(x86::rbp, offset);
        }
        auto stack = location.AsIncomingStack();
        return x86::qword_ptr(x86::rbp, StackFrameUtils::GetIncomingArgumentOffset(stack->index));
    };
    if (auto from = source.AsGpRegister()) {
        if (auto to = destination.AsGpRegister()) {
            const auto sourceRegister = RegisterUtils::Get64BitRegister(from->type);
            const auto destinationRegister = RegisterUtils::Get64BitRegister(to->type);
            if (sourceRegister.id() != destinationRegister.id()) {
                assembler.mov(destinationRegister, sourceRegister);
            }
            if (!floating && width && width != 64) {
                TypeUtils::NormalizeInteger(assembler, destinationRegister, width, type.IsSignedInteger());
            }
            return;
        }
    }
    if (auto from = source.AsSimdRegister()) {
        if (auto to = destination.AsSimdRegister()) {
            const auto sourceRegister = RegisterUtils::GetArgumentSimdRegister(from->index);
            const auto destinationRegister = RegisterUtils::GetArgumentSimdRegister(to->index);
            if (sourceRegister.id() != destinationRegister.id()) {
                if (width == 64) {
                    assembler.movsd(destinationRegister, sourceRegister);
                } else {
                    assembler.movss(destinationRegister, sourceRegister);
                }
            }
            return;
        }
    }
    if (auto from = source.AsGpRegister()) {
        const auto sourceRegister = RegisterUtils::Get64BitRegister(from->type);
        if (sourceRegister.id() != scratch.id()) {
            assembler.mov(scratch, sourceRegister);
        }
    } else if (auto simdFrom = source.AsSimdRegister()) {
        const auto sourceRegister = RegisterUtils::GetArgumentSimdRegister(simdFrom->index);
        if (width == 64) {
            assembler.movq(scratch, sourceRegister);
        } else {
            assembler.movd(scratch.r32(), sourceRegister);
        }
    } else {
        assembler.mov(scratch, getMemory(source));
    }
    if (!floating && width && width != 64) {
        TypeUtils::NormalizeInteger(assembler, scratch, width, type.IsSignedInteger());
    }
    if (auto to = destination.AsGpRegister()) {
        const auto destinationRegister = RegisterUtils::Get64BitRegister(to->type);
        if (destinationRegister.id() != scratch.id()) {
            assembler.mov(destinationRegister, scratch);
        }
    } else if (auto simdTo = destination.AsSimdRegister()) {
        const auto destinationRegister = RegisterUtils::GetArgumentSimdRegister(simdTo->index);
        if (width == 64) {
            assembler.movq(destinationRegister, scratch);
        } else {
            assembler.movd(destinationRegister, scratch.r32());
        }
    } else {
        assembler.mov(getMemory(destination), scratch);
    }
}

void MemoryUtils::StoreConstant(FunctionContext& context, uint64_t bits, const Location& destination,
    const CHIR::Type& type, const asmjit::x86::Gp& scratch)
{
    context.GetAssembler().mov(scratch, asmjit::imm(bits));
    Move(context, destination, RegisterUtils::ToLocation(scratch), type, scratch);
}
} // namespace Cangjie::JIT::X86_64
