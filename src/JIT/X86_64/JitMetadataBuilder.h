// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_JIT_METADATA_BUILDER_H
#define CANGJIE_JIT_X86_64_JIT_METADATA_BUILDER_H

#include <cstddef>
#include <array>
#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace Cangjie::JIT::X86_64 {
class JitMetadataBuilder final {
public:
    // Masks use the runtime's x86 register numbering. Slots are byte offsets
    // from RBP, multiples of eight. XMM registers cannot be derived bases.
    using Roots = std::pair<uint32_t, std::vector<int32_t>>;
    using DerivedRoots = std::vector<Roots>;
    using Result = std::variant<std::pair<std::vector<uint8_t>, std::array<uint8_t, 4>>, std::string>;

    explicit JitMetadataBuilder(bool stackGrowth, std::string name = {}, std::string directory = {},
        std::string filename = {})
        : stackGrowth(stackGrowth), name(std::move(name)), directory(std::move(directory)), filename(std::move(filename)) {}
    // Offsets are positive eight-byte slot counts, ordered RBX, R12..R15;
    // UINT32_MAX denotes an unsaved register.
    void RecordFrame(uint32_t frameSize, const std::array<uint32_t, 5>& savedOffsets);
    // Derived entries correspond to GC general registers in increasing bit
    // order, then GC slots in descending offset order. An empty vector means none.
    void RecordSafepoint(uint32_t pc, uint32_t gcRegisters, std::vector<int32_t> gcSlots,
        uint32_t stackRegisters, std::vector<int32_t> stackSlots, uint32_t line, DerivedRoots derived);
    // Null catch type denotes catch-all; empty types denote cleanup.
    size_t RecordLandingPad(uint32_t offset, std::vector<uintptr_t> catchTypes);
    void RecordCallSite(uint32_t start, uint32_t length, size_t landingPad);
    // Addresses are final executable addresses, not their writable aliases.
    // The host writes the first result at metadataAddress and the second at
    // codeAddress - 4, and keeps both allocations alive for the code lifetime.
    Result Build(uintptr_t metadataAddress, uintptr_t codeAddress, uint32_t codeSize) const;

private:
    bool stackGrowth;
    uint32_t frameSize = 0;
    bool hasFrame = false;
    std::array<uint32_t, 5> savedOffsets{UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    using Safepoint = std::tuple<uint32_t, Roots, Roots, uint32_t, DerivedRoots>;
    std::vector<Safepoint> safepoints;
    std::string name;
    std::string directory;
    std::string filename;
    std::vector<std::pair<uint32_t, std::vector<uintptr_t>>> landingPads;
    std::vector<std::tuple<uint32_t, uint32_t, size_t>> callSites;
};
} // namespace Cangjie::JIT::X86_64
#endif
