// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "ValueNumeration.h"

#include <cassert>
#include <limits>
#include <ostream>
#include <utility>

#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Value/Value.h"

namespace Cangjie::JIT::X86_64 {

ValueNumeration::ValueNumeration(std::vector<const CHIR::Value*> numberedValues, Id parameterCount)
    : parameterOffset(0),
      localOffset(parameterCount),
      endOffset(static_cast<Id>(numberedValues.size())),
      valueToId(),
      idToValue(std::move(numberedValues))
{
    assert(idToValue.size() <= std::numeric_limits<Id>::max());
    assert(parameterCount <= endOffset);
    valueToId.reserve(idToValue.size());
    for (size_t index = 0; index < idToValue.size(); ++index) {
        const bool inserted = valueToId.emplace(idToValue[index], static_cast<Id>(index)).second;
        assert(inserted);
        (void)inserted;
    }
}

std::optional<ValueNumeration::Id> ValueNumeration::GetId(const CHIR::Value* value) const noexcept
{
    const auto found = valueToId.find(value);
    if (found == valueToId.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::optional<ValueNumeration::Id> ValueNumeration::GetId(const CHIR::Parameter* parameter) const noexcept
{
    return GetId(static_cast<const CHIR::Value*>(parameter));
}

std::optional<ValueNumeration::Id> ValueNumeration::GetId(const CHIR::LocalVar* local) const noexcept
{
    return GetId(static_cast<const CHIR::Value*>(local));
}

const CHIR::Value* ValueNumeration::GetValue(Id id) const noexcept
{
    assert(id < endOffset);
    return idToValue[id];
}

void ValueNumeration::Dump(std::ostream& output) const
{
    output << "ValueNumeration(parameters=[" << parameterOffset << ", " << localOffset << "), locals=[" << localOffset
           << ", " << endOffset << "))\n";
    for (size_t index = 0; index < idToValue.size(); ++index) {
        const auto id = static_cast<Id>(index);
        output << id << ' ' << (id < localOffset ? "parameter " : "local ") << idToValue[index]->GetIdentifier()
               << '\n';
    }
}

ValueNumeration::Id ValueNumeration::GetParameterOffset() const noexcept
{
    return parameterOffset;
}

ValueNumeration::Id ValueNumeration::GetLocalOffset() const noexcept
{
    return localOffset;
}

ValueNumeration::Id ValueNumeration::GetEndOffset() const noexcept
{
    return endOffset;
}

ValueNumeration::Id ValueNumeration::GetParameterCount() const noexcept
{
    return localOffset - parameterOffset;
}

ValueNumeration::Id ValueNumeration::GetLocalCount() const noexcept
{
    return endOffset - localOffset;
}

ValueNumeration::Id ValueNumeration::GetValueCount() const noexcept
{
    return endOffset - parameterOffset;
}

ValueNumerator::ValueNumerator(const CHIR::Function& function) noexcept : function(function)
{
}

ValueNumeration ValueNumerator::Run() const
{
    const auto* body = function.GetBody();
    assert(body != nullptr);

    const auto& parameters = function.GetParams();
    assert(parameters.size() <= std::numeric_limits<ValueNumeration::Id>::max());

    std::vector<const CHIR::Value*> numberedValues;
    numberedValues.reserve(parameters.size() + function.GetExpressionsNum());
    numberedValues.insert(numberedValues.end(), parameters.begin(), parameters.end());

    for (const auto* block : body->GetAllBlocks()) {
        if (block->GetFuncOrLambdaBody() != body) {
            continue;
        }
        for (const auto* expression : block->GetExpressions()) {
            if (const auto* result = expression->GetResult(); result != nullptr) {
                numberedValues.push_back(result);
            }
        }
    }

    assert(numberedValues.size() <= std::numeric_limits<ValueNumeration::Id>::max());
    return ValueNumeration(std::move(numberedValues), static_cast<ValueNumeration::Id>(parameters.size()));
}

} // namespace Cangjie::JIT::X86_64
