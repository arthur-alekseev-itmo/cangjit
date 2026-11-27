// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "BinaryUtils.h"

#include "../X86_64/JitAssert.h"

namespace Cangjie::JIT::X86_64::BinaryUtils {
void AppendU32LE(std::vector<uint8_t>& output, uint32_t value)
{
    for (unsigned i = 0; i < sizeof(value); ++i) {
        output.push_back(static_cast<uint8_t>(value >> (i * 8)));
    }
}

void AppendU64LE(std::vector<uint8_t>& output, uint64_t value)
{
    for (unsigned i = 0; i < sizeof(value); ++i) {
        output.push_back(static_cast<uint8_t>(value >> (i * 8)));
    }
}

void PatchU32LE(std::vector<uint8_t>& output, size_t offset, uint32_t value)
{
    JIT_ASSERT(offset <= output.size() && output.size() - offset >= sizeof(value),
        "descriptor patch offset is out of range");
    for (unsigned i = 0; i < sizeof(value); ++i) {
        output[offset + i] = static_cast<uint8_t>(value >> (i * 8));
    }
}

void AppendULEB128(std::vector<uint8_t>& output, uint64_t value)
{
    do {
        const auto byte = static_cast<uint8_t>(value & 0x7f);
        value >>= 7;
        output.push_back(byte | (value != 0 ? 0x80 : 0));
    } while (value != 0);
}

BitWriter::BitWriter(std::vector<uint8_t>& output) noexcept : output(output), bitOffset(output.size() * 8)
{
}

void BitWriter::WriteBits(uint32_t value, uint32_t bitCount)
{
    for (uint32_t i = 0; i < bitCount; ++i, ++bitOffset) {
        if (bitOffset / 8 == output.size()) {
            output.push_back(0);
        }
        if ((value & (uint32_t{1} << i)) != 0) {
            output[bitOffset / 8] |= uint8_t{1} << (bitOffset % 8);
        }
    }
}

size_t BitWriter::GetBitOffset() const noexcept
{
    return bitOffset;
}
} // namespace Cangjie::JIT::X86_64::BinaryUtils
