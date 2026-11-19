// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_COMPILERS_FUNCTION_COMPILE_FUNCTION_STUB_H
#define CANGJIE_JIT_X86_64_COMPILERS_FUNCTION_COMPILE_FUNCTION_STUB_H

#include "../../Types.h"

namespace Cangjie::CHIR {
class Function;
}

namespace Cangjie::JIT::X86_64 {
class GlobalContext;

class CompileFunctionStub final {
public:
    static CodeAddress Compile(GlobalContext& context, const CHIR::Function* function);
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_COMPILERS_FUNCTION_COMPILE_FUNCTION_STUB_H
