// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_UTILS_CALLING_CONVENTION_H
#define CANGJIE_JIT_X86_64_UTILS_CALLING_CONVENTION_H

#include <optional>
#include <unordered_map>

#include "../Location.h"

namespace Cangjie::CHIR {
class Function;
class Parameter;
}

namespace Cangjie::JIT::X86_64 {
class CallingConvention final {
public:
    using ParameterLocations = std::unordered_map<const CHIR::Parameter*, Location>;

    static std::optional<ParameterLocations> GetParameterLocations(const CHIR::Function& function);
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_UTILS_CALLING_CONVENTION_H
