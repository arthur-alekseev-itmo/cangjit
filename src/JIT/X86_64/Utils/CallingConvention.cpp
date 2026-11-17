// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "CallingConvention.h"

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

std::optional<CallingConvention::ParameterLocations> CallingConvention::GetParameterLocations(
    const CHIR::Function& function)
{
    if (function.IsCFunc() || function.GetFuncType()->HasVarArg() || !function.GetReturnType()) {
        return std::nullopt;
    }
    const auto indirectReturn = HasIndirectReturn(*function.GetReturnType());
    if (!indirectReturn) {
        return std::nullopt;
    }

    size_t integerIndex = *indirectReturn ? 1 : 0;
    size_t simdIndex = 0;
    size_t stackIndex = 0;
    ParameterLocations locations;
    locations.reserve(function.GetParams().size());
    for (auto* parameter : function.GetParams()) {
        if (!parameter || !parameter->GetType()) {
            return std::nullopt;
        }
        const auto& type = *parameter->GetType();
        switch (Classify(type)) {
            case ArgumentClass::INTEGER:
                if (integerIndex < INTEGER_REGISTERS.size()) {
                    locations.emplace(parameter, Location::Register(integerIndex, INTEGER_REGISTERS[integerIndex],
                        Width(type)));
                    ++integerIndex;
                } else {
                    locations.emplace(parameter, Location::IncomingStack(stackIndex++));
                }
                break;
            case ArgumentClass::SIMD:
                if (simdIndex < SIMD_REGISTER_COUNT) {
                    locations.emplace(parameter, Location::SimdRegister(simdIndex++, Width(type)));
                } else {
                    locations.emplace(parameter, Location::IncomingStack(stackIndex++));
                }
                break;
            case ArgumentClass::UNSUPPORTED:
                return std::nullopt;
            default:
                return std::nullopt;
        }
    }
    return locations;
}
} // namespace Cangjie::JIT::X86_64
