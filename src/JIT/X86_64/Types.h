// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_TYPES_H
#define CANGJIE_JIT_X86_64_TYPES_H

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "Location.h"

namespace Cangjie::CHIR {
class Parameter;
class Type;
}

namespace Cangjie::JIT::X86_64 {
using CodeAddress = uintptr_t;

struct ParameterPlacement {
    std::unordered_map<const CHIR::Parameter*, Location> locations;
    // Types used for native ABI classification.
    std::unordered_map<const CHIR::Parameter*, const CHIR::Type*> abiParameterTypes;
    const CHIR::Type* abiReturnType = nullptr;
    std::optional<Location> sretLocation;
    std::unordered_map<const CHIR::Parameter*, Location> basePointerLocations;
    // In Function::GetGenericTypeParams() order, followed by outer and this type info.
    std::vector<Location> genericTypeInfoLocations;
    std::optional<Location> outerTypeInfoLocation;
    std::optional<Location> thisTypeInfoLocation;
    std::optional<Location> returnLocation;
    bool indirectReturn = false;
    uint32_t stackSize = 0;
};

struct CompilationError {
    std::string message;
};
using CompilationResult = std::variant<CodeAddress, CompilationError>;
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_TYPES_H
