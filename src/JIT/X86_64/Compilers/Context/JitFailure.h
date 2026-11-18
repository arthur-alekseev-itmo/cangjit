// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_JIT_FAILURE_H
#define CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_JIT_FAILURE_H

#include <cstdint>
#include <optional>

namespace Cangjie::CHIR {
enum class ExprKind : uint8_t;
}

namespace Cangjie::JIT::X86_64 {
enum class JitFailureKind : uint8_t {
#define JIT_FAILURE(name, message) name,
#include "JitFailures.inc"
#undef JIT_FAILURE
};

struct JitFailure {
    JitFailureKind kind;
    std::optional<CHIR::ExprKind> expressionKind;
};

inline const char* GetJitFailureMessage(JitFailureKind kind) noexcept
{
    switch (kind) {
#define JIT_FAILURE(name, message) case JitFailureKind::name: return message;
#include "JitFailures.inc"
#undef JIT_FAILURE
    }
    return "unknown JIT failure";
}
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_JIT_FAILURE_H
