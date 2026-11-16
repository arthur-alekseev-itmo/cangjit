// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_LOCATION_H
#define CANGJIE_JIT_X86_64_LOCATION_H

#include <cstdint>
#include <string>
#include <utility>
#include <variant>

namespace Cangjie::JIT::X86_64 {

/// Physical x86-64 general-purpose registers. Allocation policy decides which
/// registers are available; a Location can represent reserved registers too.
enum class RegisterType : uint8_t {
    RAX,
    RBX,
    RCX,
    RDX,
    RSI,
    RDI,
    RBP,
    RSP,
    R8,
    R9,
    R10,
    R11,
    R12,
    R13,
    R14,
    R15,
    COUNT,
};

/// Width of the low register view used by an instruction operand.
enum class OperandWidth : uint8_t {
    BYTE,
    WORD,
    DWORD,
    QWORD,
    COUNT,
};

/// A storage location assigned to a JIT value.
///
/// A Location is always valid: values that have not been assigned a location
/// should be represented by std::optional<Location> at the call site.
class Location final {
public:
    using Index = size_t;

    enum class Kind : uint8_t {
        REGISTER,
        STACK,
        GLOBAL,
    };

    struct RegisterLocation {
        Index index;
        RegisterType type;
        OperandWidth width;
    };

    struct StackLocation {
        Index index;
    };

    struct GlobalLocation {
        std::string name;
    };

    using Value = std::variant<RegisterLocation, StackLocation, GlobalLocation>;

    static Location Register(Index index, RegisterType type, OperandWidth width) noexcept
    {
        return Location(RegisterLocation{index, type, width});
    }

    static Location Stack(Index index) noexcept
    {
        return Location(StackLocation{index});
    }

    static Location Global(std::string name)
    {
        return Location(GlobalLocation{std::move(name)});
    }

    Kind GetKind() const noexcept
    {
        return static_cast<Kind>(value.index());
    }

    bool IsRegister() const noexcept
    {
        return std::holds_alternative<RegisterLocation>(value);
    }

    bool IsStack() const noexcept
    {
        return std::holds_alternative<StackLocation>(value);
    }

    bool IsGlobal() const noexcept
    {
        return std::holds_alternative<GlobalLocation>(value);
    }

    const RegisterLocation* AsRegister() const noexcept
    {
        return std::get_if<RegisterLocation>(&value);
    }

    const StackLocation* AsStack() const noexcept
    {
        return std::get_if<StackLocation>(&value);
    }

    const GlobalLocation* AsGlobal() const noexcept
    {
        return std::get_if<GlobalLocation>(&value);
    }

    const Value& GetValue() const noexcept
    {
        return value;
    }

private:
    explicit Location(RegisterLocation location) noexcept : value(location)
    {
    }

    explicit Location(StackLocation location) noexcept : value(location)
    {
    }

    explicit Location(GlobalLocation location) : value(std::move(location))
    {
    }

    Value value;
};

} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_LOCATION_H
