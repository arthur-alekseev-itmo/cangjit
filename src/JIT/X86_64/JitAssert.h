// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_JIT_ASSERT_H
#define CANGJIE_JIT_X86_64_JIT_ASSERT_H

#include <cstdio>
#include <cstdlib>

namespace Cangjie::JIT::X86_64 {
// Reports a violated JIT invariant or an unsupported construct; never returns.
[[noreturn]] inline void ReportJitInvariantFailure(
    const char* file, int line, const char* condition, const char* message) noexcept
{
    std::fprintf(stderr, "JIT internal error at %s:%d: %s (%s)\n", file, line, message, condition);
    std::abort();
}
} // namespace Cangjie::JIT::X86_64

// Always enabled, including release builds: a violated invariant would
// otherwise publish broken machine code.
#define JIT_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            ::Cangjie::JIT::X86_64::ReportJitInvariantFailure(__FILE__, __LINE__, #condition, message); \
        } \
    } while (false)

#define JIT_FAIL(message) JIT_ASSERT(false, message)

#endif // CANGJIE_JIT_X86_64_JIT_ASSERT_H
