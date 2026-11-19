// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_REGISTER_ALLOCATION_VALUE_NUMERATION_H
#define CANGJIE_JIT_X86_64_REGISTER_ALLOCATION_VALUE_NUMERATION_H

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <unordered_map>
#include <vector>

namespace Cangjie::CHIR {
class Function;
class LocalVar;
class Parameter;
class Value;
} // namespace Cangjie::CHIR

namespace Cangjie::JIT::X86_64 {

class ValueNumeration final {
public:
    using Id = uint32_t;
    ValueNumeration(std::vector<const CHIR::Value*> numberedValues, Id parameterCount);

    std::optional<Id> GetId(const CHIR::Value* value) const noexcept;
    std::optional<Id> GetId(const CHIR::Parameter* parameter) const noexcept;
    std::optional<Id> GetId(const CHIR::LocalVar* local) const noexcept;
    const CHIR::Value* GetValue(Id id) const noexcept;
    void Dump(std::ostream& output) const;

    Id GetParameterOffset() const noexcept;
    Id GetLocalOffset() const noexcept;
    Id GetEndOffset() const noexcept;
    Id GetParameterCount() const noexcept;
    Id GetLocalCount() const noexcept;
    Id GetValueCount() const noexcept;

private:
    Id parameterOffset;
    Id localOffset;
    Id endOffset;
    std::unordered_map<const CHIR::Value*, Id> valueToId;
    std::vector<const CHIR::Value*> idToValue;
};

class ValueNumerator final {
public:
    explicit ValueNumerator(const CHIR::Function& function) noexcept;

    ValueNumeration Run() const;

private:
    const CHIR::Function& function;
};

} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_REGISTER_ALLOCATION_VALUE_NUMERATION_H
