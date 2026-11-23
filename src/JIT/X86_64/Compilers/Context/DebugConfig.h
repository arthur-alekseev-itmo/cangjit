// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_DEBUG_CONFIG_H
#define CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_DEBUG_CONFIG_H

#include <cstdint>

namespace Cangjie::JIT::X86_64 {
enum class FuncPrintMode : uint8_t {
    NONE = 0,
    ASM = 1 << 0,
    ENTRY = 1 << 1,
    REGALLOC = 1 << 2,
    LIVENESS = 1 << 3,
};

constexpr FuncPrintMode operator|(FuncPrintMode left, FuncPrintMode right) noexcept
{
    return static_cast<FuncPrintMode>(static_cast<uint8_t>(left) | static_cast<uint8_t>(right));
}

enum class RuntimeLogMode : uint8_t { OFF, ON };

struct DebugConfig {
    FuncPrintMode func_print_mode;
    RuntimeLogMode runtime_log_mode;
};

// Edit these values and rebuild to configure JIT diagnostics.
inline constexpr DebugConfig DEFAULT_DEBUG_CONFIG{FuncPrintMode::NONE, RuntimeLogMode::ON};

constexpr bool IsEnabled(FuncPrintMode modes, FuncPrintMode mode) noexcept
{
    return (static_cast<uint8_t>(modes) & static_cast<uint8_t>(mode)) != 0;
}
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_DEBUG_CONFIG_H
