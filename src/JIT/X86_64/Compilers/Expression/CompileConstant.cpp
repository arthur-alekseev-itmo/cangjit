// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "CompileConstant.h"

#include "../../JitAssert.h"
#include "../../Utils/MemoryUtils.h"
#include "../../../Runtime/RuntimeError.h"
#include "../Context/FunctionContext.h"
#include "../Context/GlobalContext.h"
#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Value/LiteralValue.h"
#include "cangjie/CHIR/IR/Value/Value.h"
#include "cangjie/CHIR/IR/Type/Type.h"

#include <cstdint>
#include <cstring>

namespace Cangjie::JIT::X86_64 {
namespace {
uint64_t GetLiteralBits(const CHIR::LiteralValue& literal)
{
    if (literal.IsBoolLiteral()) {
        return static_cast<const CHIR::BoolLiteral&>(literal).GetVal() ? 1 : 0;
    }
    if (literal.IsIntLiteral()) {
        return static_cast<const CHIR::IntLiteral&>(literal).GetUnsignedVal();
    }
    if (literal.IsRuneLiteral()) {
        return static_cast<uint32_t>(static_cast<const CHIR::RuneLiteral&>(literal).GetVal());
    }
    if (literal.IsNullLiteral()) {
        return 0;
    }
    if (literal.IsFloatLiteral()) {
        auto type = literal.GetType();
        if (type && type->GetTypeKind() == CHIR::Type::TYPE_FLOAT32) {
            const float value = static_cast<float>(static_cast<const CHIR::FloatLiteral&>(literal).GetVal());
            uint32_t bits;
            std::memcpy(&bits, &value, sizeof(bits));
            return bits;
        }
        if (type && type->GetTypeKind() == CHIR::Type::TYPE_FLOAT64) {
            const auto value = static_cast<const CHIR::FloatLiteral&>(literal).GetVal();
            uint64_t bits;
            std::memcpy(&bits, &value, sizeof(bits));
            return bits;
        }
    }
    JIT_FAIL("unsupported literal operand");
}

uintptr_t GetGlobalAddress(FunctionContext& context, const CHIR::Value& value)
{
    JIT_ASSERT(value.IsImportedVar(), "package-local global storage is not supported");
    auto& bindings = context.GetGlobalContext().GetRuntimeBindings();
    Require(bindings.EnsurePackage(static_cast<const CHIR::GlobalVar&>(value).GetPackageName()));
    auto resolved = bindings.Resolve(value.GetIdentifierWithoutPrefix());
    Require(resolved);
    return reinterpret_cast<uintptr_t>(resolved.Value());
}
} // namespace

void CompileConstant::Compile(FunctionContext& context, const CHIR::Constant& expression)
{
    auto value = expression.GetValue();
    auto result = expression.GetResult();
    JIT_ASSERT(value && result && result->GetType(), "constant has a value and a typed result");
    auto destination = context.GetValueLocation(result);
    if (value->IsUnitLiteral()) {
        MemoryUtils::StoreConstant(context, 0, *destination, *result->GetType(), asmjit::x86::rax);
        return;
    }
    if (value->IsLiteral()) {
        const auto bits = GetLiteralBits(static_cast<const CHIR::LiteralValue&>(*value));
        MemoryUtils::StoreConstant(context, bits, *destination, *result->GetType(), asmjit::x86::rax);
        return;
    }
    if (value->IsGlobalVar()) {
        const auto address = GetGlobalAddress(context, *value);
        MemoryUtils::StoreConstant(context, address, *destination, *result->GetType(), asmjit::x86::rax);
        return;
    }
    auto source = context.GetValueLocation(value);
    MemoryUtils::Move(context, *destination, *source, *result->GetType(), asmjit::x86::r11);
}
} // namespace Cangjie::JIT::X86_64
