// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_LOCATION_H
#define CANGJIE_JIT_X86_64_LOCATION_H

#include <cstddef>
#include <cstdint>
#include <variant>

namespace Cangjie::JIT::X86_64 {

enum class GPRegisterType : uint8_t {
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

enum class OperandWidth : uint8_t {
    BYTE,
    WORD,
    DWORD,
    QWORD,
    COUNT,
};

class Location final {
public:
    using Index = size_t;

    struct GpRegisterLocation {
        Index index;
        GPRegisterType type;
        OperandWidth width;
    };

    struct StackLocation {
        Index index;
    };

    struct SimdRegisterLocation {
        Index index;
        OperandWidth width;
    };

    struct IncomingStackLocation {
        Index index;
    };

    using Value = std::variant<GpRegisterLocation, StackLocation, SimdRegisterLocation, IncomingStackLocation>;

    static Location GpRegister(Index index, GPRegisterType type, OperandWidth width) noexcept
    {
        return Location(GpRegisterLocation{index, type, width});
    }

    static Location Stack(Index index) noexcept
    {
        return Location(StackLocation{index});
    }

    static Location SimdRegister(Index index, OperandWidth width) noexcept
    {
        return Location(SimdRegisterLocation{index, width});
    }

    static Location IncomingStack(Index index) noexcept
    {
        return Location(IncomingStackLocation{index});
    }

    bool IsStack() const noexcept
    {
        return std::holds_alternative<StackLocation>(value);
    }

    const GpRegisterLocation* AsGpRegister() const noexcept
    {
        return std::get_if<GpRegisterLocation>(&value);
    }

    const StackLocation* AsStack() const noexcept
    {
        return std::get_if<StackLocation>(&value);
    }

    const SimdRegisterLocation* AsSimdRegister() const noexcept
    {
        return std::get_if<SimdRegisterLocation>(&value);
    }

    const IncomingStackLocation* AsIncomingStack() const noexcept
    {
        return std::get_if<IncomingStackLocation>(&value);
    }

private:
    explicit Location(GpRegisterLocation location) noexcept : value(location)
    {
    }

    explicit Location(StackLocation location) noexcept : value(location)
    {
    }

    explicit Location(SimdRegisterLocation location) noexcept : value(location)
    {
    }

    explicit Location(IncomingStackLocation location) noexcept : value(location)
    {
    }

    Value value;
};

} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_LOCATION_H
