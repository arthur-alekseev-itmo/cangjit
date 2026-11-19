// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "CompileBlock.h"

#include "../Expression/CompileApply.h"
#include "../Expression/CompileBinaryExpression.h"
#include "../Expression/CompileConstant.h"
#include "../Expression/CompileDebug.h"
#include "../Expression/CompileBranch.h"
#include "../Expression/CompileExit.h"
#include "../Expression/CompileGoTo.h"
#include "../Expression/CompileUnaryExpression.h"
#include "../Expression/CompileNumericCast.h"
#include "../Context/FunctionContext.h"
#include "../../Utils/MemoryUtils.h"
#include "../../Utils/RegisterUtils.h"
#include "../../Utils/StackFrameUtils.h"
#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Expression/Terminator.h"
#include "cangjie/CHIR/IR/Type/Type.h"
#include "cangjie/CHIR/IR/Value/Value.h"

namespace Cangjie::JIT::X86_64 {
namespace {
unsigned ScalarWidth(const CHIR::Type* type)
{
    return !type ? 0 : type->IsBoolean() ? 8 : RegisterUtils::IntegerWidth(*type);
}

bool MatchesReference(const CHIR::Value* reference, const CHIR::Type* type)
{
    if (!reference || !reference->GetType() || !reference->GetType()->IsRef() || !ScalarWidth(type)) {
        return false;
    }
    const auto* base = static_cast<const CHIR::RefType*>(reference->GetType())->GetBaseType();
    return base && base->GetTypeKind() == type->GetTypeKind();
}

void CompileAllocate(FunctionContext& context, const CHIR::Allocate& expression)
{
    const auto* result = expression.GetResult();
    if (!MatchesReference(result, expression.GetType())) {
        context.Fail(JitFailureKind::UnsupportedAllocation);
        return;
    }
    const auto id = context.GetValueNumeration().GetId(result);
    const auto& allocation = context.GetRegisterAllocation();
    const auto slot = id ? allocation.GetAllocationSlot(*id) : std::nullopt;
    const auto offset = slot ? StackFrameUtils::GetSlotOffset(*slot, allocation.GetStackSlotCount()) : std::nullopt;
    if (!offset) {
        context.Fail(JitFailureKind::UnsupportedAllocation);
        return;
    }
    context.GetAssembler().lea(asmjit::x86::rax, asmjit::x86::ptr(asmjit::x86::rbp, *offset));
    MemoryUtils::StoreValue(context, result, asmjit::x86::rax);
}

void CompileLoad(FunctionContext& context, const CHIR::Load& expression)
{
    const auto* result = expression.GetResult();
    const auto* type = result ? result->GetType() : nullptr;
    const auto* location = expression.GetLocation();
    if (!MatchesReference(location, type)) {
        context.Fail(JitFailureKind::UnsupportedMemoryAccess);
        return;
    }
    if (!MemoryUtils::LoadValue(context, location, asmjit::x86::rcx)) {
        return;
    }
    auto& a = context.GetAssembler();
    const auto width = ScalarWidth(type);
    switch (width) {
        case 8: a.movzx(asmjit::x86::eax, asmjit::x86::byte_ptr(asmjit::x86::rcx)); break;
        case 16: a.movzx(asmjit::x86::eax, asmjit::x86::word_ptr(asmjit::x86::rcx)); break;
        case 32: a.mov(asmjit::x86::eax, asmjit::x86::dword_ptr(asmjit::x86::rcx)); break;
        case 64: a.mov(asmjit::x86::rax, asmjit::x86::qword_ptr(asmjit::x86::rcx)); break;
        default: context.Fail(JitFailureKind::UnsupportedMemoryAccess); return;
    }
    RegisterUtils::NormalizeInteger(a, asmjit::x86::rax, width, type->IsSignedInteger());
    MemoryUtils::StoreValue(context, result, asmjit::x86::rax);
}

void CompileStore(FunctionContext& context, const CHIR::Store& expression)
{
    const auto* value = expression.GetValue();
    const auto* type = value ? value->GetType() : nullptr;
    const auto* location = expression.GetLocation();
    if (!MatchesReference(location, type)) {
        context.Fail(JitFailureKind::UnsupportedMemoryAccess);
        return;
    }
    if (!MemoryUtils::LoadValue(context, value, asmjit::x86::rax) ||
        !MemoryUtils::LoadValue(context, location, asmjit::x86::rcx)) {
        return;
    }
    auto& a = context.GetAssembler();
    switch (ScalarWidth(type)) {
        case 8: a.mov(asmjit::x86::byte_ptr(asmjit::x86::rcx), asmjit::x86::al); break;
        case 16: a.mov(asmjit::x86::word_ptr(asmjit::x86::rcx), asmjit::x86::ax); break;
        case 32: a.mov(asmjit::x86::dword_ptr(asmjit::x86::rcx), asmjit::x86::eax); break;
        case 64: a.mov(asmjit::x86::qword_ptr(asmjit::x86::rcx), asmjit::x86::rax); break;
        default: context.Fail(JitFailureKind::UnsupportedMemoryAccess); return;
    }
}
} // namespace

void CompileBlock::Compile(FunctionContext& context, const CHIR::Block& block)
{
    context.GetAssembler().bind(context.GetBlockLabel(&block));
    if (context.GetFailure() || context.GetError() != asmjit::kErrorOk) {
        return;
    }
    for (const auto* expression : block.GetExpressions()) {
        CompileExpression(context, *expression);
        if (context.GetFailure() || context.GetError() != asmjit::kErrorOk) {
            return;
        }
    }
}

void CompileBlock::CompileExpression(FunctionContext& context, const CHIR::Expression& expression)
{
    switch (expression.GetExprKind()) {
        case CHIR::ExprKind::APPLY:
            CompileApply::Compile(context, static_cast<const CHIR::Apply&>(expression));
            break;
        case CHIR::ExprKind::CONSTANT:
            CompileConstant::Compile(context, static_cast<const CHIR::Constant&>(expression));
            break;
        case CHIR::ExprKind::NUMERIC_CAST:
            CompileNumericCast::Compile(context, static_cast<const CHIR::NumericCast&>(expression));
            break;
        case CHIR::ExprKind::DEBUGEXPR:
            CompileDebug::Compile(context, static_cast<const CHIR::Debug&>(expression));
            break;
        case CHIR::ExprKind::ALLOCATE:
            CompileAllocate(context, static_cast<const CHIR::Allocate&>(expression));
            break;
        case CHIR::ExprKind::LOAD:
            CompileLoad(context, static_cast<const CHIR::Load&>(expression));
            break;
        case CHIR::ExprKind::STORE:
            CompileStore(context, static_cast<const CHIR::Store&>(expression));
            break;
        case CHIR::ExprKind::NEG:
        case CHIR::ExprKind::NOT:
        case CHIR::ExprKind::BITNOT:
            CompileUnaryExpression::Compile(context, static_cast<const CHIR::UnaryExpression&>(expression));
            break;
        case CHIR::ExprKind::ADD:
        case CHIR::ExprKind::SUB:
        case CHIR::ExprKind::MUL:
        case CHIR::ExprKind::DIV:
        case CHIR::ExprKind::MOD:
        case CHIR::ExprKind::EXP:
        case CHIR::ExprKind::LSHIFT:
        case CHIR::ExprKind::RSHIFT:
        case CHIR::ExprKind::BITAND:
        case CHIR::ExprKind::BITOR:
        case CHIR::ExprKind::BITXOR:
        case CHIR::ExprKind::LT:
        case CHIR::ExprKind::GT:
        case CHIR::ExprKind::LE:
        case CHIR::ExprKind::GE:
        case CHIR::ExprKind::EQUAL:
        case CHIR::ExprKind::NOTEQUAL:
        case CHIR::ExprKind::AND:
        case CHIR::ExprKind::OR:
            CompileBinaryExpression::Compile(context, static_cast<const CHIR::BinaryExpression&>(expression));
            break;
        case CHIR::ExprKind::GOTO:
            CompileGoTo::Compile(context, static_cast<const CHIR::GoTo&>(expression));
            break;
        case CHIR::ExprKind::BRANCH:
            CompileBranch::Compile(context, static_cast<const CHIR::Branch&>(expression));
            break;
        case CHIR::ExprKind::EXIT:
            CompileExit::Compile(context, static_cast<const CHIR::Exit&>(expression));
            break;
        default:
            context.Fail(JitFailureKind::UnsupportedExpression, expression.GetExprKind());
    }
}
} // namespace Cangjie::JIT::X86_64
