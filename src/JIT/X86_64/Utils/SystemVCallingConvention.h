// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_UTILS_SYSTEM_V_CALLING_CONVENTION_H
#define CANGJIE_JIT_X86_64_UTILS_SYSTEM_V_CALLING_CONVENTION_H

#include <optional>

#include "../Types.h"

namespace Cangjie::CHIR {
class Function;
}

namespace Cangjie::JIT::X86_64 {
class SystemVCallingConvention final {
public:
    SystemVCallingConvention() = delete;
    // The existing JIT entry/call lowering rejects supplementary operands.
    static std::optional<ParameterPlacement> GetParameterPlacement(
        const CHIR::Function& function);
    // Native function entry ABI, including supplementary operands; not the
    // separate with-TypeInfo-wrapper ABI. Override signatures are unsupported.
    static std::optional<ParameterPlacement> GetNativeParameterPlacement(
        const CHIR::Function& function);
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_UTILS_SYSTEM_V_CALLING_CONVENTION_H
