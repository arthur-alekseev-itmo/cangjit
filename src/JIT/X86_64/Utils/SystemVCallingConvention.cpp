// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "SystemVCallingConvention.h"

#include "StackFrameUtils.h"

#include <array>

#include "cangjie/CHIR/IR/Annotation.h"
#include "cangjie/CHIR/IR/Type/Type.h"
#include "cangjie/CHIR/IR/Value/Value.h"

namespace Cangjie::JIT::X86_64 {
namespace {
using Type = CHIR::Type;
constexpr std::array<RegisterType, 6> INTEGER_REGISTERS = {RegisterType::RDI, RegisterType::RSI,
    RegisterType::RDX, RegisterType::RCX, RegisterType::R8, RegisterType::R9};
constexpr size_t SIMD_REGISTER_COUNT = 8;

enum class ArgumentClass { INTEGER, SIMD, UNSUPPORTED };

ArgumentClass Classify(const Type& type, bool isC)
{
    switch (type.GetTypeKind()) {
        case Type::TYPE_INT8: case Type::TYPE_INT16: case Type::TYPE_INT32: case Type::TYPE_INT64:
        case Type::TYPE_INT_NATIVE: case Type::TYPE_UINT8: case Type::TYPE_UINT16: case Type::TYPE_UINT32:
        case Type::TYPE_UINT64: case Type::TYPE_UINT_NATIVE: case Type::TYPE_BOOLEAN:
        case Type::TYPE_CPOINTER: case Type::TYPE_CSTRING:
            return ArgumentClass::INTEGER;
        case Type::TYPE_RUNE:
            return isC ? ArgumentClass::UNSUPPORTED : ArgumentClass::INTEGER;
        case Type::TYPE_FUNC:
            return !isC || type.IsCFunc() ? ArgumentClass::INTEGER : ArgumentClass::UNSUPPORTED;
        case Type::TYPE_GENERIC:
            return isC ? ArgumentClass::UNSUPPORTED : ArgumentClass::INTEGER;
        case Type::TYPE_BOXTYPE:
            return isC ? ArgumentClass::UNSUPPORTED : ArgumentClass::INTEGER;
        case Type::TYPE_STRUCT: case Type::TYPE_TUPLE: case Type::TYPE_VARRAY:
        case Type::TYPE_UNIT: case Type::TYPE_NOTHING:
            return isC ? ArgumentClass::UNSUPPORTED : ArgumentClass::INTEGER;
        case Type::TYPE_REFTYPE: {
            const auto& args = type.GetTypeArgs();
            if (isC || args.empty() || !args[0]) {
                return ArgumentClass::UNSUPPORTED;
            }
            return ArgumentClass::INTEGER;
        }
        case Type::TYPE_FLOAT16:
            return isC ? ArgumentClass::UNSUPPORTED : ArgumentClass::SIMD;
        case Type::TYPE_FLOAT32: case Type::TYPE_FLOAT64:
            return ArgumentClass::SIMD;
        default:
            return ArgumentClass::UNSUPPORTED;
    }
}

OperandWidth Width(const Type& type)
{
    switch (type.GetTypeKind()) {
        case Type::TYPE_INT8: case Type::TYPE_UINT8: case Type::TYPE_BOOLEAN:
        case Type::TYPE_BOXTYPE:
            return OperandWidth::BYTE;
        case Type::TYPE_INT16: case Type::TYPE_UINT16: case Type::TYPE_FLOAT16:
            return OperandWidth::WORD;
        case Type::TYPE_INT32: case Type::TYPE_UINT32: case Type::TYPE_RUNE: case Type::TYPE_FLOAT32:
            return OperandWidth::DWORD;
        default:
            return OperandWidth::QWORD;
    }
}

std::optional<bool> HasIndirectReturn(const Type& type, bool isC)
{
    switch (type.GetTypeKind()) {
        case Type::TYPE_UNIT: case Type::TYPE_NOTHING:
            return !isC;
        case Type::TYPE_GENERIC:
            return isC ? std::nullopt : std::optional<bool>(true);
        case Type::TYPE_STRUCT: case Type::TYPE_TUPLE: case Type::TYPE_VARRAY:
            return isC ? std::nullopt : std::optional<bool>(true);
        case Type::TYPE_VOID:
            return false;
        default:
            if (Classify(type, isC) != ArgumentClass::UNSUPPORTED) {
                return false;
            }
            return std::nullopt;
    }
}
} // namespace

std::optional<ParameterPlacement> SystemVCallingConvention::GetNativeParameterPlacement(const CHIR::Function& function)
{
    // The native override ABI may differ from the body signature; conversion is
    // not implemented, so no consumer may treat it as callable yet.
    if (function.Get<CHIR::OverrideSrcFuncType>()) {
        return std::nullopt;
    }
    const auto* functionType = function.GetFuncType();
    if (!functionType || functionType->HasVarArg() || !functionType->GetReturnType()) {
        return std::nullopt;
    }
    const bool isC = functionType->IsCFunc();
    const auto* returnType = functionType->GetReturnType();
    const auto parameterTypes = functionType->GetParamTypes();
    const auto& parameters = function.GetParams();
    if (parameterTypes.size() != parameters.size()) {
        return std::nullopt;
    }
    const auto indirectReturn = HasIndirectReturn(*returnType, isC);
    if (!indirectReturn) {
        return std::nullopt;
    }
    size_t integerIndex = 0;
    size_t simdIndex = 0;
    size_t stackIndex = 0;
    auto nextInteger = [&](OperandWidth width) {
        if (integerIndex < INTEGER_REGISTERS.size()) {
            const size_t index = integerIndex++;
            return Location::Register(index, INTEGER_REGISTERS[index], width);
        }
        return Location::IncomingStack(stackIndex++);
    };
    ParameterPlacement placement;
    placement.abiReturnType = returnType;
    placement.indirectReturn = *indirectReturn;
    if (*indirectReturn) {
        placement.sretLocation = nextInteger(OperandWidth::QWORD);
    } else if (!returnType->IsVoid() && !(isC && (returnType->IsUnit() || returnType->IsNothing()))) {
        if (Classify(*returnType, isC) == ArgumentClass::SIMD) {
            placement.returnLocation = Location::SimdRegister(0, Width(*returnType));
        } else {
            placement.returnLocation = Location::Register(0, RegisterType::RAX, Width(*returnType));
        }
    }
    placement.locations.reserve(parameters.size());
    placement.abiParameterTypes.reserve(parameters.size());
    for (size_t i = 0; i < parameters.size(); ++i) {
        const auto* parameter = parameters[i];
        const auto* type = parameterTypes[i];
        if (!parameter || !parameter->GetType() || !type ||
            !placement.abiParameterTypes.emplace(parameter, type).second) {
            return std::nullopt;
        }
        switch (Classify(*type, isC)) {
            case ArgumentClass::INTEGER:
                if (!placement.locations.emplace(parameter, nextInteger(Width(*type))).second) {
                    return std::nullopt;
                }
                break;
            case ArgumentClass::SIMD: {
                const auto location = simdIndex < SIMD_REGISTER_COUNT
                    ? Location::SimdRegister(simdIndex++, Width(*type)) : Location::IncomingStack(stackIndex++);
                if (!placement.locations.emplace(parameter, location).second) {
                    return std::nullopt;
                }
                break;
            }
            case ArgumentClass::UNSUPPORTED:
            default:
                return std::nullopt;
        }
        if (!isC && type->IsRef()) {
            const auto& args = type->GetTypeArgs();
            if (!args.empty() && args[0] && (args[0]->IsStruct() || args[0]->IsTuple())) {
                placement.basePointerLocations.emplace(parameter, nextInteger(OperandWidth::QWORD));
            }
        }
    }
    if (!isC) {
        placement.genericTypeInfoLocations.reserve(function.GetGenericTypeParams().size());
        for (const auto* generic : function.GetGenericTypeParams()) {
            if (!generic) {
                return std::nullopt;
            }
            placement.genericTypeInfoLocations.emplace_back(nextInteger(OperandWidth::QWORD));
        }
        if (function.GetParentCustomTypeDef()) {
            placement.outerTypeInfoLocation = nextInteger(OperandWidth::QWORD);
        }
        if (function.TestAttr(CHIR::Attribute::STATIC)) {
            placement.thisTypeInfoLocation = nextInteger(OperandWidth::QWORD);
        }
    }
    const auto stackSize = StackFrameUtils::GetFrameSize(stackIndex);
    if (!stackSize) {
        return std::nullopt;
    }
    placement.stackSize = *stackSize;
    return placement;
}

std::optional<ParameterPlacement> SystemVCallingConvention::GetParameterPlacement(const CHIR::Function& function)
{
    auto placement = GetNativeParameterPlacement(function);
    if (!placement || !placement->basePointerLocations.empty() ||
        !placement->genericTypeInfoLocations.empty() || placement->outerTypeInfoLocation ||
        placement->thisTypeInfoLocation ||
        (placement->indirectReturn && !placement->abiReturnType->IsUnit() &&
            !placement->abiReturnType->IsNothing())) {
        return std::nullopt;
    }
    return placement;
}
} // namespace Cangjie::JIT::X86_64
