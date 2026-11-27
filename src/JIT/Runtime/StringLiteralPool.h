// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_STRING_LITERAL_POOL_H
#define CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_STRING_LITERAL_POOL_H

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Cangjie::JIT::X86_64 {
class JitRuntimeBindings;

class StringLiteralPool final {
public:
    explicit StringLiteralPool(JitRuntimeBindings& runtimeBindings) noexcept;

    // Addresses remain valid until the pool is destroyed. Calls are externally serialized.
    const void* GetOrCreate(std::string_view text);

private:
    void* GetByteArrayTypeInfo() const;
    const void* StoreLiteral(std::string text, void* byteArrayTypeInfo);

    JitRuntimeBindings& runtimeBindings;
    std::unordered_map<std::string, std::vector<uint64_t>> literals;
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_STRING_LITERAL_POOL_H
