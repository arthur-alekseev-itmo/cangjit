// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_RUNTIME_BINARY_UTILS_H
#define CANGJIE_JIT_RUNTIME_BINARY_UTILS_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Cangjie::JIT::X86_64::BinaryUtils {
void AppendU32LE(std::vector<uint8_t>& output, uint32_t value);
void AppendU64LE(std::vector<uint8_t>& output, uint64_t value);
bool PatchU32LE(std::vector<uint8_t>& output, size_t offset, uint32_t value);
void AppendULEB128(std::vector<uint8_t>& output, uint64_t value);

class BitWriter final {
public:
    explicit BitWriter(std::vector<uint8_t>& output) noexcept;

    // Writes the low bitCount bits, least-significant bit first; bitCount <= 32.
    void WriteBits(uint32_t value, uint32_t bitCount);
    size_t GetBitOffset() const noexcept;

private:
    std::vector<uint8_t>& output;
    size_t bitOffset;
};
} // namespace Cangjie::JIT::X86_64::BinaryUtils

#endif // CANGJIE_JIT_RUNTIME_BINARY_UTILS_H
