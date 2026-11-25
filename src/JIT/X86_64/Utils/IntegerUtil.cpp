// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "IntegerUtil.h"

#include "cangjie/CHIR/IR/Type/Type.h"

namespace Cangjie::JIT::X86_64 {
unsigned IntegerUtil::IntegerWidth(const CHIR::Type& type) noexcept
{
    using T = CHIR::Type;
    switch (type.GetTypeKind()) {
        case T::TYPE_INT8: case T::TYPE_UINT8: return 8;
        case T::TYPE_INT16: case T::TYPE_UINT16: return 16;
        case T::TYPE_INT32: case T::TYPE_UINT32: return 32;
        case T::TYPE_INT64: case T::TYPE_UINT64:
        case T::TYPE_INT_NATIVE: case T::TYPE_UINT_NATIVE: return 64;
        default: return 0;
    }
}

void IntegerUtil::NormalizeInteger(asmjit::x86::Assembler& assembler, const asmjit::x86::Gp& reg,
    unsigned width, bool isSigned)
{
    if (width != 64) {
        assembler.shl(reg, 64 - width);
        if (isSigned) {
            assembler.sar(reg, 64 - width);
        } else {
            assembler.shr(reg, 64 - width);
        }
    }
}
} // namespace Cangjie::JIT::X86_64
