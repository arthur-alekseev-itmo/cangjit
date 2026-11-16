// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_GLOBAL_CONTEXT_H
#define CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_GLOBAL_CONTEXT_H

#include <asmjit/core.h>

namespace Cangjie::JIT::X86_64 {
/// Owns executable allocations for the lifetime of a JIT execution.
class GlobalContext final {
public:
    asmjit::JitRuntime& GetRuntime() noexcept
    {
        return runtime;
    }

private:
    asmjit::JitRuntime runtime;
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_GLOBAL_CONTEXT_H
