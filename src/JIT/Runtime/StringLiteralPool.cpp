// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "StringLiteralPool.h"

#include "JitRuntimeBindings.h"
#include "../X86_64/JitAssert.h"

#include <cstring>
#include <limits>
#include <utility>

namespace Cangjie::JIT::X86_64 {
StringLiteralPool::StringLiteralPool(JitRuntimeBindings& runtimeBindings) noexcept : runtimeBindings(runtimeBindings) {}

const void* StringLiteralPool::GetOrCreate(std::string_view text)
{
    JIT_ASSERT(text.size() <= std::numeric_limits<uint32_t>::max(), "string literal fits a 32-bit length");
    std::string key(text);
    auto found = literals.find(key);
    if (found != literals.end()) {
        return found->second.data();
    }
    auto typeInfo = GetByteArrayTypeInfo();
    JIT_ASSERT(typeInfo != nullptr, "byte array type information is available");
    return StoreLiteral(std::move(key), typeInfo);
}

void* StringLiteralPool::GetByteArrayTypeInfo() const
{
    using LookupType = void* (*)(const char*);
    return reinterpret_cast<LookupType>(runtimeBindings.GetTypeByName())("RawArray<UInt8>");
}

const void* StringLiteralPool::StoreLiteral(std::string text, void* byteArrayTypeInfo)
{
    const auto size = text.size();
    std::vector<uint64_t> storage(4 + (size + sizeof(uint64_t) - 1) / sizeof(uint64_t));
    auto inserted = literals.emplace(std::move(text), std::move(storage));
    auto& words = inserted.first->second;
    // Native literal layout: String {array, start, length}, followed by
    // RawArray {type info, length, bytes}.
    words[0] = reinterpret_cast<uintptr_t>(words.data() + 2);
    words[1] = static_cast<uint64_t>(size) << 32;
    words[2] = reinterpret_cast<uintptr_t>(byteArrayTypeInfo);
    words[3] = size;
    if (size != 0) {
        std::memcpy(words.data() + 4, inserted.first->first.data(), size);
    }
    return words.data();
}
} // namespace Cangjie::JIT::X86_64
