// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_JIT_FUNCTION_METADATA_H
#define CANGJIE_JIT_X86_64_JIT_FUNCTION_METADATA_H

#include <cstddef>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Cangjie::JIT::X86_64 {
// Per-function metadata: frame and safepoint roots, exception tables and
// stack-trace strings. Serialize emits a runtime descriptor with a
// bit-packed stack map, an exception table when call sites exist, and the
// four-byte code prefix containing the descriptor's relative address.
class FunctionMetadata final {
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

    explicit FunctionMetadata(bool stackGrowth, std::string name = {}, std::string directory = {},
        std::string filename = {})
        : stackGrowth(stackGrowth), name(std::move(name)), directory(std::move(directory)),
          filename(std::move(filename)) {}
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
    // Serialize writes the descriptor at metadataAddress and the prefix at
    // codeAddress - 4; the host keeps both allocations alive for the code lifetime.
    EncodedMetadata Serialize(uintptr_t metadataAddress, uintptr_t codeAddress, uint32_t codeSize) const;

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
    using EncodedBytes = std::vector<uint8_t>;
    using EncodedPrefix = std::array<uint8_t, 4>;
    struct RootIndices {
        uint32_t registers;
        uint32_t slots;
    };
    struct LandingPad {
        uint32_t offset;
        std::vector<uintptr_t> catchTypes;
    };

    EncodedPrefix EncodePrefix(
        uintptr_t metadataAddress, uintptr_t codeAddress, uint32_t codeSize) const;
    void ValidateMetadataInputs(const std::vector<CallSite>& sites, uint32_t codeSize) const;
    EncodedBytes EncodeStackMap(std::vector<Safepoint>& points, uint32_t codeSize) const;
    EncodedBytes EncodeExceptionTable(const std::vector<CallSite>& sites) const;
    EncodedBytes EncodeDescriptor(uint32_t codeSize, const std::vector<uint8_t>& map,
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
