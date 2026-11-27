// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_TYPES_H
#define CANGJIE_JIT_X86_64_TYPES_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <variant>

namespace Cangjie::JIT::X86_64 {
template<typename T, typename Err>
class [[nodiscard]] Result {
public:
    static Result Success(T value)
    {
        return Result(std::in_place_index<0>, std::move(value));
    }

    static Result Failure(Err error)
    {
        return Result(std::in_place_index<1>, std::move(error));
    }

    bool HasValue() const noexcept { return value.index() == 0; }
    explicit operator bool() const noexcept { return HasValue(); }
    T& Value() & { return std::get<0>(value); }
    const T& Value() const& { return std::get<0>(value); }
    T&& Value() && { return std::get<0>(std::move(value)); }
    Err& Error() & { return std::get<1>(value); }
    const Err& Error() const& { return std::get<1>(value); }
    Err&& Error() && { return std::get<1>(std::move(value)); }

private:
    template<std::size_t Index, typename U>
    explicit Result(std::in_place_index_t<Index> index, U&& item)
        : value(index, std::forward<U>(item)) {}

    std::variant<T, Err> value;
};

template<typename Err>
class [[nodiscard]] Result<void, Err> {
public:
    static Result Success() { return Result(); }
    static Result Failure(Err error) { return Result(std::move(error)); }

    bool HasValue() const noexcept { return !error.has_value(); }
    explicit operator bool() const noexcept { return HasValue(); }
    Err& Error() & { return *error; }
    const Err& Error() const& { return *error; }
    Err&& Error() && { return std::move(*error); }

private:
    Result() = default;
    explicit Result(Err value) : error(std::move(value)) {}

    std::optional<Err> error;
};

using CodeAddress = uintptr_t;
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_TYPES_H
