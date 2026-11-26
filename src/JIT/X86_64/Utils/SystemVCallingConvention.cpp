// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "SystemVCallingConvention.h"

#include "StackFrameUtils.h"

#include <array>

#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Annotation.h"
#include "cangjie/CHIR/IR/Type/Type.h"
#include "cangjie/CHIR/IR/Value/Value.h"

namespace Cangjie::JIT::X86_64 {
namespace {
using Type = CHIR::Type;
constexpr std::array<GPRegisterType, 6> INTEGER_REGISTERS = {GPRegisterType::RDI, GPRegisterType::RSI,
    GPRegisterType::RDX, GPRegisterType::RCX, GPRegisterType::R8, GPRegisterType::R9};
constexpr size_t SIMD_REGISTER_COUNT = 8;

enum class ArgumentClass { INTEGER, SIMD };

ArgumentClass Classify(const Type& type, bool isC)
{
    switch (type.GetTypeKind()) {
        case Type::TYPE_FLOAT16:
            return isC ? ArgumentClass::INTEGER : ArgumentClass::SIMD;
        case Type::TYPE_FLOAT32: case Type::TYPE_FLOAT64:
            return ArgumentClass::SIMD;
        default:
            return ArgumentClass::INTEGER;
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

bool HasIndirectReturn(const Type& type, bool isC)
{
    switch (type.GetTypeKind()) {
        case Type::TYPE_UNIT: case Type::TYPE_NOTHING:
            return !isC;
        case Type::TYPE_GENERIC: case Type::TYPE_STRUCT: case Type::TYPE_TUPLE: case Type::TYPE_VARRAY:
            return !isC;
        default:
            return false;
    }
}

ParameterPlacement ComputePlacement(const CHIR::Function& function,
    const CHIR::FuncType* functionType, size_t genericCount, bool isStatic, bool allowBasePointer)
{
    const bool isC = functionType->IsCFunc();
    const auto* returnType = functionType->GetReturnType();
    const auto parameterTypes = functionType->GetParamTypes();
    const auto& parameters = function.GetParams();
    size_t integerIndex = 0;
    size_t simdIndex = 0;
    size_t stackIndex = 0;
    auto nextInteger = [&](OperandWidth width) {
        if (integerIndex < INTEGER_REGISTERS.size()) {
            const size_t index = integerIndex++;
            return Location::GpRegister(index, INTEGER_REGISTERS[index], width);
        }
        return Location::IncomingStack(stackIndex++);
    };
    ParameterPlacement placement;
    placement.abiReturnType = returnType;
    placement.indirectReturn = HasIndirectReturn(*returnType, isC);
    if (placement.indirectReturn) {
        placement.sretLocation = nextInteger(OperandWidth::QWORD);
    } else if (!returnType->IsVoid() && !(isC && (returnType->IsUnit() || returnType->IsNothing()))) {
        if (Classify(*returnType, isC) == ArgumentClass::SIMD) {
            placement.returnLocation = Location::SimdRegister(0, Width(*returnType));
        } else {
            placement.returnLocation = Location::GpRegister(0, GPRegisterType::RAX, Width(*returnType));
        }
    }
    placement.locations.reserve(parameters.size());
    placement.abiParameterTypes.reserve(parameters.size());
    for (size_t i = 0; i < parameters.size(); ++i) {
        const auto* parameter = parameters[i];
        const auto* type = parameterTypes[i];
        placement.abiParameterTypes.emplace(parameter, type);
        if (Classify(*type, isC) == ArgumentClass::SIMD) {
            const auto location = simdIndex < SIMD_REGISTER_COUNT
                ? Location::SimdRegister(simdIndex++, Width(*type)) : Location::IncomingStack(stackIndex++);
            placement.locations.emplace(parameter, location);
        } else {
            placement.locations.emplace(parameter, nextInteger(Width(*type)));
        }
        if (!isC && allowBasePointer && type->IsRef()) {
            const auto& args = type->GetTypeArgs();
            if (!args.empty() && args[0] && (args[0]->IsStruct() || args[0]->IsTuple())) {
                placement.basePointerLocations.emplace(parameter, nextInteger(OperandWidth::QWORD));
            }
        }
    }
    if (!isC) {
        placement.genericTypeInfoLocations.reserve(genericCount);
        for (size_t i = 0; i < genericCount; ++i) {
            placement.genericTypeInfoLocations.emplace_back(nextInteger(OperandWidth::QWORD));
        }
        if (function.GetParentCustomTypeDef()) {
            placement.outerTypeInfoLocation = nextInteger(OperandWidth::QWORD);
        }
        if (isStatic) {
            placement.thisTypeInfoLocation = nextInteger(OperandWidth::QWORD);
        }
    }
    placement.stackSize = *StackFrameUtils::GetFrameSize(stackIndex);
    return placement;
}
} // namespace

ParameterPlacement SystemVCallingConvention::GetNativeParameterPlacement(const CHIR::Function& function)
{
    const auto* overrideType = function.Get<CHIR::OverrideSrcFuncType>();
    const auto* functionType = overrideType ? overrideType : function.GetFuncType();
    const auto& generics = function.GetGenericTypeParams();
    return ComputePlacement(function, functionType, generics.size(), function.TestAttr(CHIR::Attribute::STATIC), true);
}

ParameterPlacement SystemVCallingConvention::GetVirtualParameterPlacement(
    const CHIR::DynamicDispatch& expression, bool isStatic)
{
    return ComputePlacement(*expression.GetCallee(), expression.GetMethodType(),
        expression.GetInstantiatedTypeArgs().size(), isStatic, !isStatic);
}

} // namespace Cangjie::JIT::X86_64
