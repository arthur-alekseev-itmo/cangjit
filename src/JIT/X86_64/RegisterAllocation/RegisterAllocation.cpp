// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "RegisterAllocation.h"

#include <cassert>
#include <ostream>
#include <utility>

namespace Cangjie::JIT::X86_64 {
namespace {
constexpr size_t REGISTER_TYPE_COUNT = static_cast<size_t>(RegisterType::COUNT);
constexpr size_t OPERAND_WIDTH_COUNT = static_cast<size_t>(OperandWidth::COUNT);

constexpr const char* REGISTER_NAMES[REGISTER_TYPE_COUNT][OPERAND_WIDTH_COUNT] = {
    {"AL", "AX", "EAX", "RAX"},
    {"BL", "BX", "EBX", "RBX"},
    {"CL", "CX", "ECX", "RCX"},
    {"DL", "DX", "EDX", "RDX"},
    {"SIL", "SI", "ESI", "RSI"},
    {"DIL", "DI", "EDI", "RDI"},
    {"BPL", "BP", "EBP", "RBP"},
    {"SPL", "SP", "ESP", "RSP"},
    {"R8B", "R8W", "R8D", "R8"},
    {"R9B", "R9W", "R9D", "R9"},
    {"R10B", "R10W", "R10D", "R10"},
    {"R11B", "R11W", "R11D", "R11"},
    {"R12B", "R12W", "R12D", "R12"},
    {"R13B", "R13W", "R13D", "R13"},
    {"R14B", "R14W", "R14D", "R14"},
    {"R15B", "R15W", "R15D", "R15"},
};

const char* GetRegisterName(RegisterType type, OperandWidth width) noexcept
{
    const auto registerIndex = static_cast<size_t>(type);
    const auto widthIndex = static_cast<size_t>(width);
    if (registerIndex >= REGISTER_TYPE_COUNT || widthIndex >= OPERAND_WIDTH_COUNT) {
        return "UNKNOWN";
    }
    return REGISTER_NAMES[registerIndex][widthIndex];
}

void DumpLocation(const Location& location, std::ostream& output)
{
    if (const auto* stack = location.AsStack(); stack != nullptr) {
        output << "STACK(" << stack->index << ')';
        return;
    }
    if (const auto* reg = location.AsRegister(); reg != nullptr) {
        output << "REG(" << reg->index << ", " << GetRegisterName(reg->type, reg->width) << ')';
        return;
    }

    if (const auto* reg = location.AsSimdRegister()) {
        output << "XMM(" << reg->index << ')';
        return;
    }
    if (const auto* stack = location.AsIncomingStack()) {
        output << "INCOMING_STACK(" << stack->index << ')';
        return;
    }

    const auto* global = location.AsGlobal();
    assert(global != nullptr);
    output << "GLOBAL(" << global->name << ')';
}
} // namespace

RegisterAllocation::RegisterAllocation(std::vector<Location> locations, size_t stackSlotCount,
    std::vector<std::optional<size_t>> allocationSlots) noexcept
    : locations(std::move(locations)), allocationSlots(std::move(allocationSlots)), stackSlotCount(stackSlotCount)
{
}

const Location& RegisterAllocation::GetLocation(ValueNumeration::Id valueId) const noexcept
{
    assert(static_cast<size_t>(valueId) < locations.size());
    return locations[valueId];
}

size_t RegisterAllocation::GetLocationCount() const noexcept
{
    return locations.size();
}

size_t RegisterAllocation::GetStackSlotCount() const noexcept
{
    return stackSlotCount;
}

std::optional<size_t> RegisterAllocation::GetAllocationSlot(ValueNumeration::Id valueId) const noexcept
{
    return valueId < allocationSlots.size() ? allocationSlots[valueId] : std::nullopt;
}

void RegisterAllocation::Dump(std::ostream& output) const
{
    output << "RegisterAllocation(values=" << locations.size() << ", stackSlots=" << stackSlotCount << ")\n";
    for (size_t index = 0; index < locations.size(); ++index) {
        output << index << ": ";
        DumpLocation(locations[index], output);
        output << '\n';
    }
}

} // namespace Cangjie::JIT::X86_64
