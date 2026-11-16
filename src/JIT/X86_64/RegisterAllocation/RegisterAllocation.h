// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_REGISTER_ALLOCATION_REGISTER_ALLOCATION_H
#define CANGJIE_JIT_X86_64_REGISTER_ALLOCATION_REGISTER_ALLOCATION_H

#include <cstddef>
#include <iosfwd>
#include <vector>

#include "../Location.h"
#include "ValueNumeration.h"

namespace Cangjie::JIT::X86_64 {

class RegisterAllocation final {
public:
    RegisterAllocation(std::vector<Location> locations, size_t stackSlotCount) noexcept;

    const Location& GetLocation(ValueNumeration::Id valueId) const noexcept;
    size_t GetLocationCount() const noexcept;
    size_t GetStackSlotCount() const noexcept;
    void Dump(std::ostream& output) const;

private:
    std::vector<Location> locations;
    size_t stackSlotCount;
};

} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_REGISTER_ALLOCATION_REGISTER_ALLOCATION_H
