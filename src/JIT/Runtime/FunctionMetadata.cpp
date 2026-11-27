// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "FunctionMetadata.h"
#include "BinaryUtils.h"

#include "../X86_64/JitAssert.h"

#include <algorithm>
#include <functional>


namespace Cangjie::JIT::X86_64 {
namespace {
void WriteStackMapVarUInt(BinaryUtils::BitWriter& writer, uint32_t value)
{
    if (value <= 11) {
        writer.WriteBits(value, 4);
        return;
    }
    const uint32_t width = value <= 0xff ? 8 : value <= 0xffff ? 16 : value <= 0xffffff ? 24 : 32;
    writer.WriteBits(11 + width / 8, 4);
    writer.WriteBits(value, width);
}
}
void FunctionMetadata::RecordFrame(uint32_t size, const std::array<uint32_t, 5>& offsets)
{
    frameSize = size;
    hasFrame = true;
    savedOffsets = offsets;
}

void FunctionMetadata::RecordSafepoint(uint32_t pc, uint32_t gcRegisters, std::vector<int32_t> gcSlots,
    uint32_t stackRegisters, std::vector<int32_t> stackSlots, uint32_t line, DerivedRoots derived)
{
    safepoints.push_back({pc, Roots{gcRegisters, std::move(gcSlots)},
        Roots{stackRegisters, std::move(stackSlots)}, line, std::move(derived)});
}

size_t FunctionMetadata::RecordLandingPad(uint32_t offset, std::vector<uintptr_t> catchTypes)
{
    landingPads.push_back({offset, std::move(catchTypes)});
    return landingPads.size() - 1;
}

void FunctionMetadata::RecordCallSite(uint32_t start, uint32_t length, size_t landingPad)
{
    callSites.push_back({start, length, landingPad});
}

FunctionMetadata::EncodedMetadata FunctionMetadata::Serialize(uintptr_t metadataAddress,
    uintptr_t codeAddress, uint32_t codeSize) const
{
    auto prefix = EncodePrefix(metadataAddress, codeAddress, codeSize);

    auto points = safepoints;
    std::sort(points.begin(), points.end(), [](const auto& a, const auto& b) { return a.pc < b.pc; });
    auto sites = callSites;
    std::sort(sites.begin(), sites.end(), [](const CallSite& a, const CallSite& b) {
        if (a.start != b.start) return a.start < b.start;
        if (a.length != b.length) return a.length < b.length;
        return a.landingPad < b.landingPad;
    });
    ValidateMetadataInputs(sites, codeSize);

    auto map = EncodeStackMap(points, codeSize);
    auto eh = EncodeExceptionTable(sites);
    auto descriptor = EncodeDescriptor(codeSize, map, eh);
    return {std::move(descriptor), prefix};
}

FunctionMetadata::EncodedPrefix FunctionMetadata::EncodePrefix(
    uintptr_t metadataAddress, uintptr_t codeAddress, uint32_t codeSize) const
{
    JIT_ASSERT(metadataAddress != 0 && (metadataAddress & 3) == 0 && codeAddress >= 4 && codeSize != 0,
        "invalid metadata or code address/size");
    JIT_ASSERT(hasFrame, "frame information was not recorded");
    const uintptr_t prefix = codeAddress - 4;
    int64_t relative;
    if (metadataAddress >= prefix) {
        JIT_ASSERT(metadataAddress - prefix <= INT32_MAX, "descriptor is out of range");
        relative = static_cast<int64_t>(metadataAddress - prefix);
    } else {
        JIT_ASSERT(prefix - metadataAddress <= uint64_t{INT32_MAX} + 1, "descriptor is out of range");
        relative = -static_cast<int64_t>(prefix - metadataAddress);
    }
    JIT_ASSERT(relative != 0, "descriptor overlaps code prefix");
    std::array<uint8_t, 4> prefixBytes{};
    for (unsigned i = 0; i < 4; ++i) prefixBytes[i] = static_cast<uint8_t>(static_cast<uint32_t>(relative) >> (8 * i));

    return prefixBytes;
}

void FunctionMetadata::ValidateMetadataInputs(const std::vector<CallSite>& sites,
    uint32_t codeSize) const
{
    uint64_t end = 0;
    for (const auto& site : sites) {
        JIT_ASSERT(site.length != 0 && site.start >= end && uint64_t{site.start} + site.length <= codeSize &&
            site.landingPad < landingPads.size(), "invalid or overlapping call sites");
        end = uint64_t{site.start} + site.length;
        JIT_ASSERT(landingPads[site.landingPad].offset != 0 && landingPads[site.landingPad].offset < codeSize,
            "invalid landing pad offset");
    }
    for (const auto text : {&name, &directory, &filename}) {
        JIT_ASSERT(text->find_first_of(",\0", 0, 2) == std::string::npos, "invalid stack trace string");
    }
}

FunctionMetadata::EncodedBytes FunctionMetadata::EncodeStackMap(std::vector<Safepoint>& points,
    uint32_t codeSize) const
{
    // Table indices are one based. Each root set becomes a row; sharing is
    // unnecessary and preserving root order makes derived pairs unambiguous.
    std::vector<uint32_t> regs;
    std::vector<std::vector<int32_t>> slots;
    std::vector<uint32_t> lines;
    std::vector<RootIndices> derivatives;
    using Row = std::array<uint32_t, 7>;
    std::vector<Row> rows;
    auto addRoots = [&](const Roots& roots) -> RootIndices {
        uint32_t reg = 0, slot = 0;
        if (roots.registers != 0) { regs.push_back(roots.registers); reg = static_cast<uint32_t>(regs.size()); }
        if (!roots.slots.empty()) { slots.push_back(roots.slots); slot = static_cast<uint32_t>(slots.size()); }
        return {reg, slot};
    };
    uint32_t lastPc = 0;
    bool first = true;
    for (auto& point : points) {
        JIT_ASSERT(point.pc <= codeSize && (first || point.pc != lastPc), "invalid or duplicate safepoint PC");
        first = false;
        lastPc = point.pc;
        JIT_ASSERT(stackGrowth || (point.stackRoots.registers == 0 && point.stackRoots.slots.empty()),
            "stack roots require stack growth");
        auto normalize = [](Roots& roots) {
            std::sort(roots.slots.begin(), roots.slots.end(), std::greater<int32_t>());
            return std::adjacent_find(roots.slots.begin(), roots.slots.end()) == roots.slots.end() &&
                std::all_of(roots.slots.begin(), roots.slots.end(), [](int32_t n) { return n % 8 == 0; });
        };
        JIT_ASSERT(normalize(point.gcRoots) && normalize(point.stackRoots), "invalid root slots");
        size_t baseCount = static_cast<size_t>(__builtin_popcount(point.gcRoots.registers & 0xffffu)) + point.gcRoots.slots.size();
        JIT_ASSERT(point.derivedRoots.empty() || point.derivedRoots.size() == baseCount,
            "derived roots do not match bases");
        auto gcIdx = addRoots(point.gcRoots);
        auto stackIdx = addRoots(point.stackRoots);
        lines.push_back(point.line);
        uint32_t derivedIdx = 0;
        if (!point.derivedRoots.empty()) {
            derivedIdx = static_cast<uint32_t>(derivatives.size() + 1);
            for (auto& d : point.derivedRoots) {
                JIT_ASSERT(normalize(d), "invalid derived slots");
                derivatives.push_back(addRoots(d));
            }
        }
        rows.push_back({point.pc, gcIdx.registers, gcIdx.slots, static_cast<uint32_t>(lines.size()), derivedIdx,
            stackIdx.registers, stackIdx.slots});
    }
    auto bitsNeeded = [](uint32_t value) -> uint32_t {
        return value == 0 ? 0 : 32 - static_cast<uint32_t>(__builtin_clz(value));
    };
    uint32_t regIndexBits = bitsNeeded(static_cast<uint32_t>(regs.size()));
    uint32_t slotIndexBits = bitsNeeded(static_cast<uint32_t>(slots.size()));
    uint32_t lineIndexBits = bitsNeeded(static_cast<uint32_t>(lines.size()));
    uint32_t derivedIndexBits = bitsNeeded(static_cast<uint32_t>(derivatives.size()));
    uint32_t stackRegBits = stackGrowth ? regIndexBits : 0;
    uint32_t stackSlotBits = stackGrowth ? slotIndexBits : 0;
    uint32_t rowBits = regIndexBits + slotIndexBits + lineIndexBits + derivedIndexBits + stackRegBits + stackSlotBits;
    // The runtime supports bit-aligned rows, so padding the final column is
    // optional. Match LLVM's byte-aligned PC layout.
    if (stackGrowth) stackSlotBits += (8 - rowBits % 8) % 8;
    else derivedIndexBits += (8 - rowBits % 8) % 8;
    uint32_t regBits = 0, slotBits = 1, lineBits = 0;
    for (auto r : regs) regBits = std::max(regBits, bitsNeeded(r));
    for (auto l : lines) lineBits = std::max(lineBits, bitsNeeded(l));
    for (const auto& s : slots) {
        int64_t width = (int64_t{s.front()} - s.back()) / 8 + 1;
        slotBits = std::max(slotBits, static_cast<uint32_t>(width));
    }
    std::vector<uint8_t> map;
    BinaryUtils::BitWriter writer(map);
    WriteStackMapVarUInt(writer, frameSize);
    WriteStackMapVarUInt(writer, 0);
    uint32_t savedMask = 0;
    for (unsigned i = 0; i < savedOffsets.size(); ++i) {
        if (savedOffsets[i] != UINT32_MAX) savedMask |= 1u << i;
    }
    WriteStackMapVarUInt(writer, savedMask);
    for (auto offset : savedOffsets) {
        if (offset != UINT32_MAX) WriteStackMapVarUInt(writer, offset);
    }
    WriteStackMapVarUInt(writer, static_cast<uint32_t>(rows.size()));
    WriteStackMapVarUInt(writer, regIndexBits);
    WriteStackMapVarUInt(writer, slotIndexBits);
    WriteStackMapVarUInt(writer, lineIndexBits);
    WriteStackMapVarUInt(writer, derivedIndexBits);
    if (stackGrowth) {
        WriteStackMapVarUInt(writer, stackRegBits);
        WriteStackMapVarUInt(writer, stackSlotBits);
    }
    const uint32_t padding = static_cast<uint32_t>((8 - (writer.GetBitOffset() + 4) % 8) % 8);
    WriteStackMapVarUInt(writer, padding);
    writer.WriteBits(0, padding);
    for (const auto& row : rows) {
        writer.WriteBits(row[0], 32);
        writer.WriteBits(row[1], regIndexBits);
        writer.WriteBits(row[2], slotIndexBits);
        writer.WriteBits(row[3], lineIndexBits);
        writer.WriteBits(row[4], derivedIndexBits);
        if (stackGrowth) {
            writer.WriteBits(row[5], stackRegBits);
            writer.WriteBits(row[6], stackSlotBits);
        }
    }
    WriteStackMapVarUInt(writer, static_cast<uint32_t>(regs.size()));
    WriteStackMapVarUInt(writer, regBits);
    for (auto r : regs) writer.WriteBits(r, regBits);
    WriteStackMapVarUInt(writer, static_cast<uint32_t>(slots.size()));
    WriteStackMapVarUInt(writer, 32);
    WriteStackMapVarUInt(writer, slotBits);
    for (const auto& s : slots) {
        writer.WriteBits(static_cast<uint32_t>(s.front()), 32);
        size_t pos = 0;
        for (uint32_t i = 0; i < slotBits; ++i) {
            const bool set = pos < s.size() && int64_t{s.front()} - int64_t{i} * 8 == s[pos];
            writer.WriteBits(set ? 1 : 0, 1);
            if (set) ++pos;
        }
    }
    WriteStackMapVarUInt(writer, static_cast<uint32_t>(lines.size()));
    WriteStackMapVarUInt(writer, lineBits);
    for (auto l : lines) writer.WriteBits(l, lineBits);
    WriteStackMapVarUInt(writer, static_cast<uint32_t>(derivatives.size()));
    for (auto d : derivatives) {
        writer.WriteBits(d.registers, regIndexBits);
        writer.WriteBits(d.slots, slotIndexBits);
    }
    // Runtime bit reads may access four bytes beyond the final field.
    map.resize(map.size() + 5, 0);

    return map;
}

FunctionMetadata::EncodedBytes FunctionMetadata::EncodeExceptionTable(const std::vector<CallSite>& sites) const
{
    std::vector<uint8_t> eh, callTable, actions;
    std::vector<uintptr_t> types;
    for (const auto& site : sites) {
        const auto& [offset, catches] = landingPads[site.landingPad];
        uint64_t action = catches.empty() ? 0 : actions.size() + 1;
        BinaryUtils::AppendULEB128(callTable, site.start);
        BinaryUtils::AppendULEB128(callTable, site.length);
        BinaryUtils::AppendULEB128(callTable, offset);
        BinaryUtils::AppendULEB128(callTable, action);
        for (size_t i = 0; i < catches.size(); ++i) {
            // The runtime table indexes types with one byte.
            JIT_ASSERT(types.size() < 255, "too many exception types");
            types.push_back(catches[i]);
            actions.push_back(static_cast<uint8_t>(types.size()));
            actions.push_back(i + 1 == catches.size() ? 0 : 1);
        }
    }
    if (!sites.empty()) {
        eh.push_back(0xff); eh.push_back(0); // omitted LPStart, absolute pointer type table
        std::vector<uint8_t> tail;
        tail.push_back(1); // ULEB128 call-site encoding
        BinaryUtils::AppendULEB128(tail, callTable.size());
        tail.insert(tail.end(), callTable.begin(), callTable.end());
        tail.insert(tail.end(), actions.begin(), actions.end());
        for (auto it = types.rbegin(); it != types.rend(); ++it) {
            BinaryUtils::AppendU64LE(tail, static_cast<uint64_t>(*it));
        }
        BinaryUtils::AppendULEB128(eh, tail.size());
        eh.insert(eh.end(), tail.begin(), tail.end());
    }

    return eh;
}

FunctionMetadata::EncodedBytes FunctionMetadata::EncodeDescriptor(uint32_t codeSize,
    const std::vector<uint8_t>& map, const std::vector<uint8_t>& eh) const
{
    // Descriptor followed by the trace flag, dictionary and encoded strings.
    // One dictionary entry per nonempty string avoids any mangling assumptions.
    std::vector<uint8_t> result;
    for (unsigned i = 0; i < 7; ++i) BinaryUtils::AppendU32LE(result, 0);
    result.insert(result.end(), 3, 0);
    result.push_back(1); // default stack-trace format
    size_t dict = result.size();
    result.resize(result.size() + 8, 0);
    uint32_t count = 0;
    std::string dictionary;
    std::array<uint32_t, 3> offsets{};
    unsigned index = 0;
    for (const auto text : {&name, &directory, &filename}) {
        offsets[index++] = static_cast<uint32_t>(result.size());
        if (!text->empty()) {
            if (count != 0) dictionary.push_back(',');
            dictionary += *text;
            result.push_back(static_cast<uint8_t>(++count));
        }
        result.push_back(0);
    }
    BinaryUtils::PatchU32LE(result, dict, count);
    BinaryUtils::PatchU32LE(result, dict + 4, static_cast<uint32_t>(result.size() - dict));
    result.insert(result.end(), dictionary.begin(), dictionary.end());
    result.push_back(0);
    BinaryUtils::PatchU32LE(result, 4, codeSize);
    BinaryUtils::PatchU32LE(result, 8, offsets[0]);
    BinaryUtils::PatchU32LE(result, 12, offsets[1]);
    BinaryUtils::PatchU32LE(result, 16, offsets[2]);
    BinaryUtils::PatchU32LE(result, 20, static_cast<uint32_t>(dict));
    JIT_ASSERT(result.size() <= INT32_MAX, "metadata too large");
    BinaryUtils::PatchU32LE(result, 0, static_cast<uint32_t>(result.size()));
    result.insert(result.end(), map.begin(), map.end());
    if (!eh.empty()) {
        JIT_ASSERT(result.size() <= INT32_MAX, "metadata too large");
        BinaryUtils::PatchU32LE(result, 24, static_cast<uint32_t>(result.size() - 24));
        result.insert(result.end(), eh.begin(), eh.end());
    }
    return result;
}
} // namespace Cangjie::JIT::X86_64
