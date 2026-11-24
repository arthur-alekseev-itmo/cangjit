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
#include <utility>
#include <vector>

#include "../X86_64/Types.h"

namespace Cangjie::JIT::X86_64 {
// Emits a runtime descriptor with stack-trace strings, a bit-packed stack map
// and, when call sites exist, an exception table. Build also returns the
// four-byte code prefix containing the descriptor's relative address.
class JitMetadataBuilder final {
public:
    // Masks use the runtime's x86 register numbering. Slots are byte offsets
    // from RBP, multiples of eight. XMM registers cannot be derived bases.
    struct Roots {
        uint32_t registers = 0;
        std::vector<int32_t> slots;
    };
    using DerivedRoots = std::vector<Roots>;

    struct EncodedMetadata {
        std::vector<uint8_t> descriptor;
        std::array<uint8_t, 4> prefix;
    };
    using BuildResult = Result<EncodedMetadata, std::string>;

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
    // The host writes the descriptor at metadataAddress and the prefix at
    // codeAddress - 4, and keeps both allocations alive for the code lifetime.
    BuildResult Build(uintptr_t metadataAddress, uintptr_t codeAddress, uint32_t codeSize) const;

private:
    struct Safepoint {
        uint32_t pc;
        Roots gcRoots;
        Roots stackRoots;
        uint32_t line;
        DerivedRoots derivedRoots;
    };
    struct CallSite {
        uint32_t start;
        uint32_t length;
        size_t landingPad;
    };
    using EncodedBytes = Result<std::vector<uint8_t>, std::string>;
    using EncodedPrefix = Result<std::array<uint8_t, 4>, std::string>;
    struct RootIndices {
        uint32_t registers;
        uint32_t slots;
    };
    struct LandingPad {
        uint32_t offset;
        std::vector<uintptr_t> catchTypes;
    };

    EncodedPrefix BuildPrefix(
        uintptr_t metadataAddress, uintptr_t codeAddress, uint32_t codeSize) const;
    Result<void, std::string> ValidateMetadataInputs(const std::vector<CallSite>& sites, uint32_t codeSize) const;
    EncodedBytes BuildStackMap(std::vector<Safepoint>& points, uint32_t codeSize) const;
    EncodedBytes BuildExceptionTable(const std::vector<CallSite>& sites) const;
    EncodedBytes BuildDescriptor(uint32_t codeSize, const std::vector<uint8_t>& map,
        const std::vector<uint8_t>& eh) const;

    bool stackGrowth;
    uint32_t frameSize = 0;
    bool hasFrame = false;
    std::array<uint32_t, 5> savedOffsets{UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    std::vector<Safepoint> safepoints;
    std::string name;
    std::string directory;
    std::string filename;
    std::vector<LandingPad> landingPads;
    std::vector<CallSite> callSites;
};
} // namespace Cangjie::JIT::X86_64
#endif
