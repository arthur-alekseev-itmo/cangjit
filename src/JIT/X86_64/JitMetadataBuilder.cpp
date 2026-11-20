// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "JitMetadataBuilder.h"

#include <algorithm>
#include <functional>

namespace Cangjie::JIT::X86_64 {
void JitMetadataBuilder::RecordFrame(uint32_t size, const std::array<uint32_t, 5>& offsets)
{
    frameSize = size;
    hasFrame = true;
    savedOffsets = offsets;
}

void JitMetadataBuilder::RecordSafepoint(uint32_t pc, uint32_t gcRegisters, std::vector<int32_t> gcSlots,
    uint32_t stackRegisters, std::vector<int32_t> stackSlots, uint32_t line, DerivedRoots derived)
{
    safepoints.emplace_back(pc, Roots{gcRegisters, std::move(gcSlots)},
        Roots{stackRegisters, std::move(stackSlots)}, line, std::move(derived));
}

size_t JitMetadataBuilder::RecordLandingPad(uint32_t offset, std::vector<uintptr_t> catchTypes)
{
    landingPads.emplace_back(offset, std::move(catchTypes));
    return landingPads.size() - 1;
}

void JitMetadataBuilder::RecordCallSite(uint32_t start, uint32_t length, size_t landingPad)
{
    callSites.emplace_back(start, length, landingPad);
}

JitMetadataBuilder::Result JitMetadataBuilder::Build(uintptr_t metadataAddress, uintptr_t codeAddress,
    uint32_t codeSize) const
{
    if (metadataAddress == 0 || (metadataAddress & 3) != 0 || codeAddress < 4 || codeSize == 0) {
        return std::string("invalid metadata or code address/size");
    }
    if (!hasFrame) return std::string("frame information was not recorded");
    const uintptr_t prefix = codeAddress - 4;
    int64_t relative;
    if (metadataAddress >= prefix) {
        if (metadataAddress - prefix > INT32_MAX) return std::string("descriptor is out of range");
        relative = static_cast<int64_t>(metadataAddress - prefix);
    } else {
        if (prefix - metadataAddress > uint64_t{INT32_MAX} + 1) return std::string("descriptor is out of range");
        relative = -static_cast<int64_t>(prefix - metadataAddress);
    }
    if (relative == 0) return std::string("descriptor overlaps code prefix");
    std::array<uint8_t, 4> prefixBytes{};
    for (unsigned i = 0; i < 4; ++i) prefixBytes[i] = static_cast<uint8_t>(static_cast<uint32_t>(relative) >> (8 * i));

    auto points = safepoints;
    std::sort(points.begin(), points.end(), [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });
    auto sites = callSites;
    std::sort(sites.begin(), sites.end());
    uint64_t end = 0;
    for (const auto& [start, length, pad] : sites) {
        if (length == 0 || start < end || uint64_t{start} + length > codeSize || pad >= landingPads.size()) {
            return std::string("invalid or overlapping call sites");
        }
        end = uint64_t{start} + length;
        if (landingPads[pad].first == 0 || landingPads[pad].first >= codeSize) {
            return std::string("invalid landing pad offset");
        }
    }
    for (const auto* text : {&name, &directory, &filename}) {
        if (text->find_first_of(",\0", 0, 2) != std::string::npos) return std::string("invalid stack trace string");
    }

    // Table indices are one based. Each root set becomes a row; sharing is
    // unnecessary and preserving root order makes derived pairs unambiguous.
    std::vector<uint32_t> regs;
    std::vector<std::vector<int32_t>> slots;
    std::vector<uint32_t> lines;
    std::vector<std::pair<uint32_t, uint32_t>> derivatives;
    using Row = std::array<uint32_t, 7>;
    std::vector<Row> rows;
    auto addRoots = [&](const Roots& roots) -> std::pair<uint32_t, uint32_t> {
        uint32_t reg = 0, slot = 0;
        if (roots.first != 0) { regs.push_back(roots.first); reg = static_cast<uint32_t>(regs.size()); }
        if (!roots.second.empty()) { slots.push_back(roots.second); slot = static_cast<uint32_t>(slots.size()); }
        return {reg, slot};
    };
    uint32_t lastPc = 0;
    bool first = true;
    for (auto& point : points) {
        auto& [pc, gc, stack, line, derived] = point;
        if (pc > codeSize || (!first && pc == lastPc)) return std::string("invalid or duplicate safepoint PC");
        first = false;
        lastPc = pc;
        if (!stackGrowth && (stack.first != 0 || !stack.second.empty())) return std::string("stack roots require stack growth");
        auto normalize = [](Roots& roots) {
            std::sort(roots.second.begin(), roots.second.end(), std::greater<int32_t>());
            return std::adjacent_find(roots.second.begin(), roots.second.end()) == roots.second.end() &&
                std::all_of(roots.second.begin(), roots.second.end(), [](int32_t n) { return n % 8 == 0; });
        };
        if (!normalize(gc) || !normalize(stack)) return std::string("invalid root slots");
        size_t baseCount = static_cast<size_t>(__builtin_popcount(gc.first & 0xffffu)) + gc.second.size();
        if (!derived.empty() && derived.size() != baseCount) return std::string("derived roots do not match bases");
        auto gcIdx = addRoots(gc);
        auto stackIdx = addRoots(stack);
        lines.push_back(line);
        uint32_t derivedIdx = 0;
        if (!derived.empty()) {
            derivedIdx = static_cast<uint32_t>(derivatives.size() + 1);
            for (auto& d : derived) {
                if (!normalize(d)) return std::string("invalid derived slots");
                derivatives.push_back(addRoots(d));
            }
        }
        rows.push_back({pc, gcIdx.first, gcIdx.second, static_cast<uint32_t>(lines.size()), derivedIdx,
            stackIdx.first, stackIdx.second});
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
        if (width > UINT32_MAX) return std::string("slot bitmap too large");
        slotBits = std::max(slotBits, static_cast<uint32_t>(width));
    }
    std::vector<uint8_t> map;
    size_t bit = 0;
    auto writeBits = [&](uint32_t value, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i, ++bit) {
            if (bit / 8 == map.size()) map.push_back(0);
            if (i < 32 && (value & (uint32_t{1} << i))) map[bit / 8] |= uint8_t{1} << (bit % 8);
        }
    };
    auto var = [&](uint32_t value) {
        if (value <= 11) { writeBits(value, 4); return; }
        uint32_t width = value <= 0xff ? 8 : value <= 0xffff ? 16 : value <= 0xffffff ? 24 : 32;
        writeBits(11 + width / 8, 4);
        writeBits(value, width);
    };
    var(frameSize); var(0);
    uint32_t savedMask = 0;
    for (unsigned i = 0; i < savedOffsets.size(); ++i) if (savedOffsets[i] != UINT32_MAX) savedMask |= 1u << i;
    var(savedMask);
    for (auto offset : savedOffsets) if (offset != UINT32_MAX) var(offset);
    var(static_cast<uint32_t>(rows.size()));
    var(regIndexBits); var(slotIndexBits); var(lineIndexBits); var(derivedIndexBits);
    if (stackGrowth) { var(stackRegBits); var(stackSlotBits); }
    uint32_t padding = static_cast<uint32_t>((8 - (bit + 4) % 8) % 8);
    var(padding); writeBits(0, padding);
    for (const auto& row : rows) {
        writeBits(row[0], 32); writeBits(row[1], regIndexBits); writeBits(row[2], slotIndexBits);
        writeBits(row[3], lineIndexBits); writeBits(row[4], derivedIndexBits);
        if (stackGrowth) { writeBits(row[5], stackRegBits); writeBits(row[6], stackSlotBits); }
    }
    var(static_cast<uint32_t>(regs.size())); var(regBits);
    for (auto r : regs) writeBits(r, regBits);
    var(static_cast<uint32_t>(slots.size())); var(32); var(slotBits);
    for (const auto& s : slots) {
        writeBits(static_cast<uint32_t>(s.front()), 32);
        size_t pos = 0;
        for (uint32_t i = 0; i < slotBits; ++i) {
            bool set = pos < s.size() && int64_t{s.front()} - int64_t{i} * 8 == s[pos];
            writeBits(set ? 1 : 0, 1);
            if (set) ++pos;
        }
    }
    var(static_cast<uint32_t>(lines.size())); var(lineBits);
    for (auto l : lines) writeBits(l, lineBits);
    var(static_cast<uint32_t>(derivatives.size()));
    for (auto d : derivatives) { writeBits(d.first, regIndexBits); writeBits(d.second, slotIndexBits); }
    // Runtime bit reads may access four bytes beyond the final field.
    map.resize(map.size() + 5, 0);

    std::vector<uint8_t> eh, callTable, actions;
    std::vector<uintptr_t> types;
    auto uleb = [](std::vector<uint8_t>& out, uint64_t v) {
        do { uint8_t b = v & 0x7f; v >>= 7; out.push_back(b | (v ? 0x80 : 0)); } while (v);
    };
    for (const auto& [start, length, pad] : sites) {
        const auto& [offset, catches] = landingPads[pad];
        uint64_t action = catches.empty() ? 0 : actions.size() + 1;
        uleb(callTable, start); uleb(callTable, length); uleb(callTable, offset); uleb(callTable, action);
        for (size_t i = 0; i < catches.size(); ++i) {
            if (types.size() == 255) return std::string("too many exception types");
            types.push_back(catches[i]);
            actions.push_back(static_cast<uint8_t>(types.size()));
            actions.push_back(i + 1 == catches.size() ? 0 : 1);
        }
    }
    if (!sites.empty()) {
        eh.push_back(0xff); eh.push_back(0); // omitted LPStart, absolute pointer type table
        std::vector<uint8_t> tail;
        tail.push_back(1); // ULEB128 call-site encoding
        uleb(tail, callTable.size());
        tail.insert(tail.end(), callTable.begin(), callTable.end());
        tail.insert(tail.end(), actions.begin(), actions.end());
        for (auto it = types.rbegin(); it != types.rend(); ++it) {
            for (unsigned i = 0; i < 8; ++i) tail.push_back(static_cast<uint8_t>(*it >> (i * 8)));
        }
        uleb(eh, tail.size());
        eh.insert(eh.end(), tail.begin(), tail.end());
    }

    // Descriptor followed by the trace flag, dictionary and encoded strings.
    // One dictionary entry per nonempty string avoids any mangling assumptions.
    std::vector<uint8_t> result(28, 0);
    auto put32 = [&](size_t at, uint32_t v) {
        for (unsigned i = 0; i < 4; ++i) result[at + i] = static_cast<uint8_t>(v >> (8 * i));
    };
    result.insert(result.end(), 3, 0);
    result.push_back(1); // default stack-trace format
    size_t dict = result.size();
    result.resize(result.size() + 8, 0);
    uint32_t count = 0;
    std::string dictionary;
    std::array<uint32_t, 3> offsets{};
    unsigned index = 0;
    for (const auto* text : {&name, &directory, &filename}) {
        offsets[index++] = static_cast<uint32_t>(result.size());
        if (!text->empty()) {
            if (count != 0) dictionary.push_back(',');
            dictionary += *text;
            result.push_back(static_cast<uint8_t>(++count));
        }
        result.push_back(0);
    }
    put32(dict, count);
    put32(dict + 4, static_cast<uint32_t>(result.size() - dict));
    result.insert(result.end(), dictionary.begin(), dictionary.end());
    result.push_back(0);
    put32(4, codeSize); put32(8, offsets[0]); put32(12, offsets[1]); put32(16, offsets[2]);
    put32(20, static_cast<uint32_t>(dict));
    if (result.size() > INT32_MAX) return std::string("metadata too large");
    put32(0, static_cast<uint32_t>(result.size()));
    result.insert(result.end(), map.begin(), map.end());
    if (!eh.empty()) {
        if (result.size() > INT32_MAX) return std::string("metadata too large");
        put32(24, static_cast<uint32_t>(result.size() - 24));
        result.insert(result.end(), eh.begin(), eh.end());
    }
    return std::make_pair(std::move(result), prefixBytes);
}
} // namespace Cangjie::JIT::X86_64
