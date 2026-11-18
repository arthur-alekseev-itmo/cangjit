// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "CallingConventionUtils.h"

#include <array>

#include "cangjie/CHIR/IR/Type/Type.h"
#include "cangjie/CHIR/IR/Value/Value.h"

namespace Cangjie::JIT::X86_64 {
std::optional<ParameterPlacement> CallingConventionUtils::GetSysVParameterPlacement(const CHIR::Function& function)
{
    const auto* returnType = function.GetReturnType();
    if (!returnType || !(returnType->IsInteger() || returnType->IsUnit() || returnType->IsVoid())) {
        return std::nullopt;
    }

    static constexpr std::array<RegisterType, 6> argumentRegisters = {
        RegisterType::RDI, RegisterType::RSI, RegisterType::RDX,
        RegisterType::RCX, RegisterType::R8, RegisterType::R9};
    const auto& parameters = function.GetParams();
    ParameterPlacement placement;
    placement.reserve(parameters.size());
    for (size_t i = 0; i < parameters.size(); ++i) {
        auto* parameter = parameters[i];
        if (!parameter || !parameter->GetType() || !parameter->GetType()->IsInteger()) {
            return std::nullopt;
        }
        auto location = i < argumentRegisters.size()
            ? Location::Register(i, argumentRegisters[i], OperandWidth::QWORD)
            : Location::Stack(i - argumentRegisters.size());
        if (!placement.emplace(parameter, std::move(location)).second) {
            return std::nullopt;
        }
    }
    return placement;
}
} // namespace Cangjie::JIT::X86_64
