// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_UTILS_INTEGER_UTIL_H
#define CANGJIE_JIT_X86_64_UTILS_INTEGER_UTIL_H

#include <asmjit/x86.h>

namespace Cangjie::CHIR {
class Type;
}

namespace Cangjie::JIT::X86_64 {
class IntegerUtil final {
public:
    static unsigned IntegerWidth(const CHIR::Type& type) noexcept;
    static void NormalizeInteger(asmjit::x86::Assembler& assembler, const asmjit::x86::Gp& reg,
        unsigned width, bool isSigned);
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_UTILS_INTEGER_UTIL_H
