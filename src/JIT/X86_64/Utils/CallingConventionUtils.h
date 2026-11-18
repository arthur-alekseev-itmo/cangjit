// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_UTILS_CALLING_CONVENTION_UTILS_H
#define CANGJIE_JIT_X86_64_UTILS_CALLING_CONVENTION_UTILS_H

#include <optional>
#include <unordered_map>

#include "../Location.h"

namespace Cangjie::CHIR {
class Function;
class Parameter;
}

namespace Cangjie::JIT::X86_64 {
using ParameterPlacement = std::unordered_map<CHIR::Parameter*, Location>;

class CallingConventionUtils final {
public:
    // System V AMD64 scalar integer calls only. A missing result means the
    // signature cannot be represented by this helper. Parameter pointers are
    // borrowed from function and remain valid only while its parameters live.
    //
    // Stack locations describe outgoing arguments, not local stack slots: index
    // zero is at [rsp] immediately before call (or [rbp+16] after the usual
    // callee prologue), and each successive index is eight bytes higher. Do not
    // pass these locations to MemoryUtils, which addresses local frame slots.
    static std::optional<ParameterPlacement> GetSysVParameterPlacement(const CHIR::Function& function);
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_UTILS_CALLING_CONVENTION_UTILS_H
