// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "RegisterUtils.h"

#include <array>


namespace Cangjie::JIT::X86_64 {
std::optional<asmjit::x86::Gpq> RegisterUtils::Get64BitRegister(RegisterType type) noexcept
{
    namespace x86 = asmjit::x86;
    static const std::array<x86::Gpq, static_cast<size_t>(RegisterType::COUNT)> registers = {
        x86::rax, x86::rbx, x86::rcx, x86::rdx, x86::rsi, x86::rdi, x86::rbp, x86::rsp,
        x86::r8, x86::r9, x86::r10, x86::r11, x86::r12, x86::r13, x86::r14, x86::r15};
    const auto index = static_cast<size_t>(type);
    if (index >= registers.size() || type == RegisterType::RBP || type == RegisterType::RSP) {
        return std::nullopt;
    }
    return registers[index];
}

std::optional<asmjit::x86::Xmm> RegisterUtils::GetArgumentSimdRegister(size_t index) noexcept
{
    namespace x86 = asmjit::x86;
    static const std::array<x86::Xmm, 8> registers = {
        x86::xmm0, x86::xmm1, x86::xmm2, x86::xmm3, x86::xmm4, x86::xmm5, x86::xmm6, x86::xmm7};
    return index < registers.size() ? std::optional<x86::Xmm>(registers[index]) : std::nullopt;
}

bool RegisterUtils::IsDataRegister(const asmjit::x86::Gp& reg) noexcept
{
    return reg.isGpq() && reg.id() <= asmjit::x86::Gp::kIdR15 &&
        reg.id() != asmjit::x86::Gp::kIdBp && reg.id() != asmjit::x86::Gp::kIdSp;
}

} // namespace Cangjie::JIT::X86_64
