// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "RegisterUtils.h"

#include <array>

#include "../JitAssert.h"

namespace Cangjie::JIT::X86_64 {
asmjit::x86::Gpq RegisterUtils::Get64BitRegister(GPRegisterType type) noexcept
{
    namespace x86 = asmjit::x86;
    static const std::array<x86::Gpq, static_cast<size_t>(GPRegisterType::COUNT)> registers = {
        x86::rax, x86::rbx, x86::rcx, x86::rdx, x86::rsi, x86::rdi, x86::rbp, x86::rsp,
        x86::r8, x86::r9, x86::r10, x86::r11, x86::r12, x86::r13, x86::r14, x86::r15};
    const auto index = static_cast<size_t>(type);
    JIT_ASSERT(index < registers.size() && type != GPRegisterType::RBP && type != GPRegisterType::RSP,
        "64-bit data register is expected");
    return registers[index];
}

asmjit::x86::Xmm RegisterUtils::GetArgumentSimdRegister(size_t index) noexcept
{
    namespace x86 = asmjit::x86;
    static const std::array<x86::Xmm, 8> registers = {
        x86::xmm0, x86::xmm1, x86::xmm2, x86::xmm3, x86::xmm4, x86::xmm5, x86::xmm6, x86::xmm7};
    JIT_ASSERT(index < registers.size(), "argument SIMD register index is out of range");
    return registers[index];
}

Location RegisterUtils::ToLocation(const asmjit::x86::Gp& reg) noexcept
{
    static constexpr std::array<GPRegisterType, 16> types = {
        GPRegisterType::RAX, GPRegisterType::RCX, GPRegisterType::RDX, GPRegisterType::RBX,
        GPRegisterType::RSP, GPRegisterType::RBP, GPRegisterType::RSI, GPRegisterType::RDI,
        GPRegisterType::R8, GPRegisterType::R9, GPRegisterType::R10, GPRegisterType::R11,
        GPRegisterType::R12, GPRegisterType::R13, GPRegisterType::R14, GPRegisterType::R15};
    return Location::GpRegister(reg.id(), types[reg.id()], OperandWidth::QWORD);
}

Location RegisterUtils::ToLocation(const asmjit::x86::Xmm& reg) noexcept
{
    return Location::SimdRegister(reg.id(), OperandWidth::QWORD);
}

bool RegisterUtils::IsDataRegister(const asmjit::x86::Gp& reg) noexcept
{
    return reg.isGpq() && reg.id() <= asmjit::x86::Gp::kIdR15 &&
        reg.id() != asmjit::x86::Gp::kIdBp && reg.id() != asmjit::x86::Gp::kIdSp;
}

} // namespace Cangjie::JIT::X86_64
