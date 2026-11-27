// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_JIT_SYMBOLS_H
#define CANGJIE_JIT_JIT_SYMBOLS_H

#include <string>
#include <string_view>

namespace Cangjie::JIT {
inline std::string JitSlotName(std::string_view prefix, std::string_view identifier)
{
    static constexpr char HEX[] = "0123456789abcdef";
    std::string result(prefix);
    result.reserve(prefix.size() + identifier.size() * 2);
    for (char value : identifier) {
        const auto byte = static_cast<unsigned char>(value);
        result.push_back(HEX[byte >> 4]);
        result.push_back(HEX[byte & 0x0f]);
    }
    return result;
}

inline std::string JitFunctionSlotName(std::string_view identifier)
{
    return JitSlotName("__cangjie_jit_function_", identifier);
}

inline std::string JitGlobalSlotName(std::string_view identifier)
{
    return JitSlotName("__cangjie_jit_global_", identifier);
}
} // namespace Cangjie::JIT

#endif // CANGJIE_JIT_JIT_SYMBOLS_H
