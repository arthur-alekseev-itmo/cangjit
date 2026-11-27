// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_COMPILERS_COMPILE_NUMERIC_CAST_H
#define CANGJIE_JIT_X86_64_COMPILERS_COMPILE_NUMERIC_CAST_H

namespace Cangjie::CHIR {
class NumericCastBase;
class Block;
class Value;
}

namespace Cangjie::JIT::X86_64 {
class FunctionContext;

class CompileNumericCast final {
public:
    static void Compile(FunctionContext& context, const CHIR::NumericCastBase& expression,
        const CHIR::Block* normal = nullptr, const CHIR::Block* unwind = nullptr);

private:
    static void CompileFloatCast(FunctionContext& context, const CHIR::Value* source, const CHIR::Value* result,
        bool needsConversion, bool targetIsFloat64, const CHIR::Block* normal);
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_COMPILERS_COMPILE_NUMERIC_CAST_H
