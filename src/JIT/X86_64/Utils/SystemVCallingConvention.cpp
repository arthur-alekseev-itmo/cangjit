// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "SystemVCallingConvention.h"

#include "StackFrameUtils.h"

#include <array>

#include "cangjie/CHIR/IR/Type/Type.h"
#include "cangjie/CHIR/IR/Value/Value.h"

namespace Cangjie::JIT::X86_64 {
namespace {
using Type = CHIR::Type;
constexpr std::array<RegisterType, 6> INTEGER_REGISTERS = {RegisterType::RDI, RegisterType::RSI,
    RegisterType::RDX, RegisterType::RCX, RegisterType::R8, RegisterType::R9};
constexpr size_t SIMD_REGISTER_COUNT = 8;

enum class ArgumentClass { INTEGER, SIMD, UNSUPPORTED };

ArgumentClass Classify(const Type& type)
{
    switch (type.GetTypeKind()) {
        case Type::TYPE_INT8: case Type::TYPE_INT16: case Type::TYPE_INT32: case Type::TYPE_INT64:
        case Type::TYPE_INT_NATIVE: case Type::TYPE_UINT8: case Type::TYPE_UINT16: case Type::TYPE_UINT32:
        case Type::TYPE_UINT64: case Type::TYPE_UINT_NATIVE: case Type::TYPE_RUNE: case Type::TYPE_BOOLEAN:
        case Type::TYPE_CPOINTER: case Type::TYPE_CSTRING:
        case Type::TYPE_STRUCT: case Type::TYPE_TUPLE: case Type::TYPE_VARRAY:
        case Type::TYPE_UNIT: case Type::TYPE_NOTHING:
            return ArgumentClass::INTEGER;
        case Type::TYPE_REFTYPE: {
            const auto& args = type.GetTypeArgs();
            if (args.empty() || args[0]->IsStruct() || args[0]->IsTuple()) {
                return ArgumentClass::UNSUPPORTED;
            }
            return ArgumentClass::INTEGER;
        }
        case Type::TYPE_FLOAT16: case Type::TYPE_FLOAT32: case Type::TYPE_FLOAT64:
            return ArgumentClass::SIMD;
        default:
            return ArgumentClass::UNSUPPORTED;
    }
}

OperandWidth Width(const Type& type)
{
    switch (type.GetTypeKind()) {
        case Type::TYPE_INT8: case Type::TYPE_UINT8: case Type::TYPE_INT16: case Type::TYPE_UINT16:
        case Type::TYPE_INT32: case Type::TYPE_UINT32: case Type::TYPE_RUNE: case Type::TYPE_BOOLEAN:
        case Type::TYPE_FLOAT32:
            return OperandWidth::DWORD;
        case Type::TYPE_FLOAT16:
            return OperandWidth::WORD;
        default:
            return OperandWidth::QWORD;
    }
}

std::optional<bool> HasIndirectReturn(const Type& type)
{
    switch (type.GetTypeKind()) {
        case Type::TYPE_UNIT: case Type::TYPE_NOTHING: case Type::TYPE_STRUCT:
        case Type::TYPE_TUPLE: case Type::TYPE_VARRAY:
            return true;
        case Type::TYPE_VOID:
            return false;
        default:
            if (Classify(type) != ArgumentClass::UNSUPPORTED) {
                return false;
            }
            return std::nullopt;
    }
}
} // namespace

std::optional<ParameterPlacement> SystemVCallingConvention::GetParameterPlacement(const CHIR::Function& function)
{
    if (function.IsCFunc() || !function.GetFuncType() || function.GetFuncType()->HasVarArg() ||
        !function.GetReturnType()) {
        return std::nullopt;
    }
    const auto indirectReturn = HasIndirectReturn(*function.GetReturnType());
    if (!indirectReturn) {
        return std::nullopt;
    }
    size_t integerIndex = *indirectReturn ? 1 : 0;
    size_t simdIndex = 0;
    size_t stackIndex = 0;
    ParameterPlacement placement;
    placement.locations.reserve(function.GetParams().size());
    for (const auto* parameter : function.GetParams()) {
        if (!parameter || !parameter->GetType()) {
            return std::nullopt;
        }
        const auto& type = *parameter->GetType();
        switch (Classify(type)) {
            case ArgumentClass::INTEGER:
                if (integerIndex < INTEGER_REGISTERS.size()) {
                    if (!placement.locations.emplace(parameter,
                        Location::Register(integerIndex, INTEGER_REGISTERS[integerIndex], Width(type))).second) {
                        return std::nullopt;
                    }
                    ++integerIndex;
                } else if (!placement.locations.emplace(parameter, Location::IncomingStack(stackIndex++)).second) {
                    return std::nullopt;
                }
                break;
            case ArgumentClass::SIMD:
                if (simdIndex < SIMD_REGISTER_COUNT) {
                    if (!placement.locations.emplace(parameter, Location::SimdRegister(simdIndex++, Width(type))).second) {
                        return std::nullopt;
                    }
                } else if (!placement.locations.emplace(parameter, Location::IncomingStack(stackIndex++)).second) {
                    return std::nullopt;
                }
                break;
            case ArgumentClass::UNSUPPORTED:
            default:
                return std::nullopt;
        }
    }
    const auto stackSize = StackFrameUtils::GetFrameSize(stackIndex);
    if (!stackSize) {
        return std::nullopt;
    }
    placement.stackSize = *stackSize;
    return placement;
}
} // namespace Cangjie::JIT::X86_64
