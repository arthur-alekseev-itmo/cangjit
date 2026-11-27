// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "CompileBlock.h"

#include <algorithm>

#include "../Context/GlobalContext.h"
#include "../Expression/CompileApply.h"
#include "../Expression/CompileBinaryExpression.h"
#include "../Expression/CompileConstant.h"
#include "../Expression/CompileDebug.h"
#include "../Expression/CompileBranch.h"
#include "../Expression/CompileExit.h"
#include "../Expression/CompileGoTo.h"
#include "../Expression/CompileInvoke.h"
#include "../Expression/CompileUnaryExpression.h"
#include "../Expression/CompileNumericCast.h"
#include "../Expression/CompileObject.h"
#include "../Context/FunctionContext.h"
#include "../../Utils/MemoryUtils.h"
#include "../../Utils/TypeUtils.h"
#include "../../Utils/RegisterUtils.h"
#include "../../Utils/StackFrameUtils.h"
#include "cangjie/Basic/UGTypeKind.h"
#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Expression/Terminator.h"
#include "cangjie/CHIR/IR/Type/ClassDef.h"
#include "cangjie/CHIR/IR/Type/Type.h"
#include "cangjie/CHIR/IR/Value/Value.h"
#include "cangjie/Mangle/CHIRTypeManglingUtils.h"

namespace Cangjie::JIT::X86_64 {
namespace {
bool IsLocalClassReference(const CHIR::Type* type)
{
    if (!type || !type->IsRef()) {
        return false;
    }
    auto base = static_cast<const CHIR::RefType*>(type)->GetBaseType();
    return base && base->IsClass() && base->IsMustLocalRegion();
}

size_t ScalarWidth(const CHIR::Type* type)
{
    if (!type) {
        return 0;
    }
    if (IsLocalClassReference(type)) {
        return 64;
    }
    if (type->IsRef() || type->IsCPointer() || type->IsCString()) {
        return 0;
    }
    return TypeUtils::TypeWidth(*type);
}

bool MatchesReference(const CHIR::Value* reference, const CHIR::Type* type)
{
    if (!reference || !reference->GetType() || !reference->GetType()->IsRef() || !type ||
        (!ScalarWidth(type) && !type->IsUnit())) {
        return false;
    }
    auto base = static_cast<const CHIR::RefType*>(reference->GetType())->GetBaseType();
    if (IsLocalClassReference(type)) {
        auto expr = reference->IsLocalVar() ? static_cast<const CHIR::LocalVar*>(reference)->GetExpr() : nullptr;
        return base == type && expr && expr->GetExprKind() == CHIR::ExprKind::ALLOCATE;
    }
    return base && base->GetTypeKind() == type->GetTypeKind();
}

void CompileAllocate(FunctionContext& context, const CHIR::Allocate& expression)
{
    auto result = expression.GetResult();
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
    auto resultLocation = context.GetValueLocation(result);
    if (!MemoryUtils::Move(context, *resultLocation, RegisterUtils::ToLocation(asmjit::x86::rax),
        *result->GetType(), asmjit::x86::r11)) {
        return;
    }
}

void CompileLoad(FunctionContext& context, const CHIR::Load& expression)
{
    auto result = expression.GetResult();
    auto type = result ? result->GetType() : nullptr;
    auto location = expression.GetLocation();
    if (!MatchesReference(location, type)) {
        context.Fail(JitFailureKind::UnsupportedMemoryAccess);
        return;
    }
    if (type->IsUnit()) {
        context.GetAssembler().xor_(asmjit::x86::eax, asmjit::x86::eax);
        auto resultLocation = context.GetValueLocation(result);
        if (!MemoryUtils::Move(context, *resultLocation,
            RegisterUtils::ToLocation(asmjit::x86::rax), *result->GetType(), asmjit::x86::r11)) {
            return;
        }
        return;
    }
    auto sourceLocation = context.GetValueLocation(location);
    if (!MemoryUtils::Move(context, RegisterUtils::ToLocation(asmjit::x86::rcx),
        *sourceLocation, *location->GetType(), asmjit::x86::r11)) {
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
    TypeUtils::NormalizeInteger(a, asmjit::x86::rax, width, type->IsSignedInteger());
    const auto resultLocation = context.GetValueLocation(result);
    if (!MemoryUtils::Move(context, *resultLocation, RegisterUtils::ToLocation(asmjit::x86::rax),
        *result->GetType(), asmjit::x86::r11)) {
        return;
    }
}

void CompileStore(FunctionContext& context, const CHIR::Store& expression)
{
    const auto value = expression.GetValue();
    const auto type = value ? value->GetType() : nullptr;
    const auto location = expression.GetLocation();
    if (!MatchesReference(location, type)) {
        context.Fail(JitFailureKind::UnsupportedMemoryAccess);
        return;
    }
    if (type->IsUnit()) {
        return;
    }
    const auto valueLocation = context.GetValueLocation(value);
    const auto referenceLocation = context.GetValueLocation(location);
    if (
        !MemoryUtils::Move(context, RegisterUtils::ToLocation(asmjit::x86::rax), *valueLocation,
            *value->GetType(), asmjit::x86::r11) ||
        !MemoryUtils::Move(context, RegisterUtils::ToLocation(asmjit::x86::rcx), *referenceLocation,
            *location->GetType(), asmjit::x86::r11)) {
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
void* ResolveExceptionSymbol(FunctionContext& context, SymbolResult symbol)
{
    if (!symbol) {
        context.Fail(JitFailureKind::UnresolvedSymbol);
        return nullptr;
    }
    return symbol.Value();
}

void CompileRTTI(FunctionContext& context, const CHIR::GetRTTI& expression)
{
    auto sourceLocation = context.GetValueLocation(expression.GetOperand());
    if (!MemoryUtils::Move(context, RegisterUtils::ToLocation(asmjit::x86::rax),
        *sourceLocation, *expression.GetOperand()->GetType(), asmjit::x86::r11)) {
        return;
    }
    auto& a = context.GetAssembler();
    a.mov(asmjit::x86::rax, asmjit::x86::qword_ptr(asmjit::x86::rax));
    a.shl(asmjit::x86::rax, 16);
    a.shr(asmjit::x86::rax, 16);
    auto resultLocation = context.GetValueLocation(expression.GetResult());
    if (!MemoryUtils::Move(context, *resultLocation, RegisterUtils::ToLocation(asmjit::x86::rax),
        *expression.GetResult()->GetType(), asmjit::x86::r11)) {
        return;
    }
}

void CompileRTTI(FunctionContext& context, const CHIR::GetRTTIStatic& expression)
{
    auto type = expression.GetRTTIType();
    if (!type) {
        context.Fail(JitFailureKind::UnsupportedExpression, expression.GetExprKind());
        return;
    }
    if (!context.GetTypeInfoEmitter().LoadTypeInfo(*type, asmjit::x86::rax)) {
        return;
    }
    auto resultLocation = context.GetValueLocation(expression.GetResult());
    if (!MemoryUtils::Move(context, *resultLocation, RegisterUtils::ToLocation(asmjit::x86::rax),
        *expression.GetResult()->GetType(), asmjit::x86::r11)) {
        return;
    }
}

void CompileInstanceOf(FunctionContext& context, const CHIR::InstanceOf& expression)
{
    auto object = expression.GetObject();
    auto objectType = object ? object->GetType() : nullptr;
    auto targetType = expression.GetType();
    auto result = expression.GetResult();
    if (!objectType || !objectType->IsRef() || !targetType ||
        !result || !result->GetType() || !result->GetType()->IsBoolean()) {
        context.Fail(JitFailureKind::UnsupportedExpression, expression.GetExprKind());
        return;
    }
    auto objectClass = static_cast<const CHIR::RefType*>(objectType)->GetBaseType();
    auto targetBase = targetType->IsRef()
        ? static_cast<const CHIR::RefType*>(targetType)->GetBaseType() : targetType;
    if (!objectClass || !objectClass->IsClass() || !targetBase) {
        context.Fail(JitFailureKind::UnsupportedExpression, expression.GetExprKind());
        return;
    }
    auto& bindings = context.GetGlobalContext().GetRuntimeBindings();
    auto subtype = ResolveExceptionSymbol(context, bindings.GetIsSubType());
    auto tupleTest = objectClass->IsAny() ? ResolveExceptionSymbol(context, bindings.GetIsTupleTypeOf()) : nullptr;
    if (!subtype || (objectClass->IsAny() && !tupleTest) ||
        !context.GetTypeInfoEmitter().LoadTypeInfo(*targetBase, asmjit::x86::rsi)) {
        return;
    }
    auto objectLocation = context.GetValueLocation(object);
    if (!MemoryUtils::Move(context, RegisterUtils::ToLocation(asmjit::x86::rdi),
        *objectLocation, *object->GetType(), asmjit::x86::r11)) {
        return;
    }
    auto& a = context.GetAssembler();
    const auto done = a.newLabel();
    a.xor_(asmjit::x86::eax, asmjit::x86::eax);
    a.test(asmjit::x86::rdi, asmjit::x86::rdi);
    a.jz(done);
    a.mov(asmjit::x86::rax, asmjit::x86::qword_ptr(asmjit::x86::rdi));
    // The high 16 bits of an object header hold runtime state, not TypeInfo.
    a.shl(asmjit::x86::rax, 16);
    a.shr(asmjit::x86::rax, 16);
    if (objectClass->IsAny()) {
        const auto nonTuple = a.newLabel();
        const auto tested = a.newLabel();
        // x86-64 native TypeInfo.type is the byte after its name pointer.
        a.cmp(asmjit::x86::byte_ptr(asmjit::x86::rax, 8), static_cast<uint8_t>(UG_TUPLE));
        a.jne(nonTuple);
        a.mov(asmjit::x86::rdx, asmjit::x86::rsi);
        a.xor_(asmjit::x86::esi, asmjit::x86::esi);
        a.long_().mov(asmjit::x86::r11, asmjit::imm(reinterpret_cast<uintptr_t>(tupleTest)));
        a.call(asmjit::x86::r11);
        a.jmp(tested);
        a.bind(nonTuple);
        a.mov(asmjit::x86::rdi, asmjit::x86::rax);
        a.long_().mov(asmjit::x86::r11, asmjit::imm(reinterpret_cast<uintptr_t>(subtype)));
        a.call(asmjit::x86::r11);
        a.bind(tested);
    } else {
        a.mov(asmjit::x86::rdi, asmjit::x86::rax);
        a.long_().mov(asmjit::x86::r11, asmjit::imm(reinterpret_cast<uintptr_t>(subtype)));
        a.call(asmjit::x86::r11);
    }
    a.movzx(asmjit::x86::eax, asmjit::x86::al);
    a.bind(done);
    auto resultLocation = context.GetValueLocation(result);
    if (!MemoryUtils::Move(context, *resultLocation, RegisterUtils::ToLocation(asmjit::x86::rax),
        *result->GetType(), asmjit::x86::r11)) {
        return;
    }
}

void CompileGetException(FunctionContext& context, const CHIR::GetException& expression)
{
    auto target = ResolveExceptionSymbol(context, context.GetGlobalContext().GetRuntimeBindings().GetExceptionWrapper());
    if (!target) {
        return;
    }
    auto& a = context.GetAssembler();
    a.long_().mov(asmjit::x86::r11, asmjit::imm(reinterpret_cast<uintptr_t>(target)));
    a.call(asmjit::x86::r11);
    if (context.GetSafepointRecorder().Record(expression)) {
        auto resultLocation = context.GetValueLocation(expression.GetResult());
        if (!MemoryUtils::Move(context, *resultLocation,
            RegisterUtils::ToLocation(asmjit::x86::rax), *expression.GetResult()->GetType(), asmjit::x86::r11)) {
            return;
        }
    }
}

void CompileBeginCatch(FunctionContext& context, const CHIR::IntrinsicBase& expression,
    const CHIR::Block* normal = nullptr, const CHIR::Block* unwind = nullptr)
{
    if (expression.GetIntrinsicKind() != CHIR::IntrinsicKind::BEGIN_CATCH ||
        expression.GetSuccessorIndex(0) != 1) {
        context.Fail(JitFailureKind::UnsupportedExpression, expression.GetExprKind());
        return;
    }
    auto target = ResolveExceptionSymbol(context, context.GetGlobalContext().GetRuntimeBindings().GetPostThrowException());
    auto operandLocation = context.GetValueLocation(expression.GetOperand(0));
    if (!target ||
        !MemoryUtils::Move(context, RegisterUtils::ToLocation(asmjit::x86::rdi), *operandLocation,
            *expression.GetOperand(0)->GetType(), asmjit::x86::r11)) {
        return;
    }
    auto& a = context.GetAssembler();
    a.long_().mov(asmjit::x86::r11, asmjit::imm(reinterpret_cast<uintptr_t>(target)));
    const auto callStart = a.offset();
    a.call(asmjit::x86::r11);
    const auto callEnd = a.offset();
    if (context.GetSafepointRecorder().Record(expression)) {
        auto resultLocation = context.GetValueLocation(expression.GetResult());
        if (!MemoryUtils::Move(context, *resultLocation,
            RegisterUtils::ToLocation(asmjit::x86::rax), *expression.GetResult()->GetType(), asmjit::x86::r11)) {
            return;
        }
    }
    if (normal && unwind && !context.GetFailure()) {
        a.jmp(context.GetBlockLabel(normal));
        context.GetExceptionLandingEmitter().Emit(*unwind, callStart, callEnd);
    }
}

void CompileRaiseException(FunctionContext& context, const CHIR::RaiseException& expression)
{
    auto value = expression.GetExceptionValue();
    if (!value || !value->GetType() || (!value->GetType()->IsRef() && !value->GetType()->IsGeneric())) {
        context.Fail(JitFailureKind::UnsupportedExpression, expression.GetExprKind());
        return;
    }
    auto target = ResolveExceptionSymbol(context, context.GetGlobalContext().GetRuntimeBindings().GetThrowException());
    auto valueLocation = context.GetValueLocation(value);
    if (!target ||
        !MemoryUtils::Move(context, RegisterUtils::ToLocation(asmjit::x86::rdi), *valueLocation,
            *value->GetType(), asmjit::x86::r11)) {
        return;
    }
    auto& a = context.GetAssembler();
    a.long_().mov(asmjit::x86::r11, asmjit::imm(reinterpret_cast<uintptr_t>(target)));
    const auto callStart = a.offset();
    a.call(asmjit::x86::r11);
    const auto callEnd = a.offset();
    if (!context.GetSafepointRecorder().Record(expression)) {
        return;
    }
    a.ud2();
    if (auto block = expression.GetExceptionBlock()) {
        context.GetExceptionLandingEmitter().Emit(*block, callStart, callEnd);
    }
}

} // namespace

void CompileBlock::Compile(FunctionContext& context, const CHIR::Block& block)
{
    context.GetAssembler().bind(context.GetBlockLabel(&block));
    if (context.GetFailure() || context.GetError() != asmjit::kErrorOk) {
        return;
    }
    const auto expressions = block.GetExpressions();
    for (size_t i = 0; i < expressions.size(); ++i) {
        CompileExpression(context, *expressions[i]);
        if (context.GetFailure() || context.GetError() != asmjit::kErrorOk) {
            return;
        }
    }
}

void CompileBlock::CompileExpression(FunctionContext& context, const CHIR::Expression& expression)
{
    if (context.GetCode().logger()) {
        auto text = expression.ToString(0);
        std::replace(text.begin(), text.end(), '\n', ' ');
        context.GetAssembler().commentf("; CHIR: %s", text.c_str());
    }
    switch (expression.GetExprKind()) {
        case CHIR::ExprKind::APPLY:
            CompileApply::Compile(context, static_cast<const CHIR::Apply&>(expression));
            break;
        case CHIR::ExprKind::TRY_APPLY:
            CompileApply::Compile(context, static_cast<const CHIR::TryApply&>(expression));
            break;
        case CHIR::ExprKind::INVOKE:
            CompileInvoke::Compile(context, static_cast<const CHIR::Invoke&>(expression));
            break;
        case CHIR::ExprKind::TRY_INVOKE:
            CompileInvoke::Compile(context, static_cast<const CHIR::TryInvoke&>(expression));
            break;
        case CHIR::ExprKind::INVOKESTATIC:
            CompileInvoke::Compile(context, static_cast<const CHIR::InvokeStatic&>(expression));
            break;
        case CHIR::ExprKind::TRY_INVOKESTATIC:
            CompileInvoke::Compile(context, static_cast<const CHIR::TryInvokeStatic&>(expression));
            break;
        case CHIR::ExprKind::GET_RTTI:
            CompileRTTI(context, static_cast<const CHIR::GetRTTI&>(expression));
            break;
        case CHIR::ExprKind::GET_RTTI_STATIC:
            CompileRTTI(context, static_cast<const CHIR::GetRTTIStatic&>(expression));
            break;
        case CHIR::ExprKind::GET_EXCEPTION:
            CompileGetException(context, static_cast<const CHIR::GetException&>(expression));
            break;
        case CHIR::ExprKind::INSTANCEOF:
            CompileInstanceOf(context, static_cast<const CHIR::InstanceOf&>(expression));
            break;
        case CHIR::ExprKind::CLASS_STATIC_CAST: {
            const auto& cast = static_cast<const CHIR::ClassStaticCast&>(expression);
            auto source = cast.GetSourceType();
            auto target = cast.GetTargetType();
            auto sourceBase = source && source->IsRef()
                ? static_cast<const CHIR::RefType*>(source)->GetBaseType() : nullptr;
            auto targetBase = target && target->IsRef()
                ? static_cast<const CHIR::RefType*>(target)->GetBaseType() : nullptr;
            if (!sourceBase || !sourceBase->IsClass() || !targetBase || !targetBase->IsClass()) {
                context.Fail(JitFailureKind::UnsupportedExpression, expression.GetExprKind());
                break;
            }
            auto sourceLocation = context.GetValueLocation(cast.GetSourceValue());
            auto resultLocation = context.GetValueLocation(cast.GetResult());
            if (
                MemoryUtils::Move(context, RegisterUtils::ToLocation(asmjit::x86::rax), *sourceLocation,
                    *cast.GetSourceValue()->GetType(), asmjit::x86::r11)) {
                MemoryUtils::Move(context, *resultLocation, RegisterUtils::ToLocation(asmjit::x86::rax),
                    *cast.GetResult()->GetType(), asmjit::x86::r11);
            }
            break;
        }
        case CHIR::ExprKind::INTRINSIC:
            CompileBeginCatch(context, static_cast<const CHIR::Intrinsic&>(expression));
            break;
        case CHIR::ExprKind::TRY_INTRINSIC: {
            const auto& intrinsic = static_cast<const CHIR::TryIntrinsic&>(expression);
            if (!intrinsic.GetSuccessBlock() || !intrinsic.GetErrorBlock()) {
                context.Fail(JitFailureKind::MissingBranchDestination);
                break;
            }
            CompileBeginCatch(context, intrinsic, intrinsic.GetSuccessBlock(), intrinsic.GetErrorBlock());
            break;
        }
        case CHIR::ExprKind::RAISE_EXCEPTION:
            CompileRaiseException(context, static_cast<const CHIR::RaiseException&>(expression));
            break;
        case CHIR::ExprKind::CONSTANT:
            CompileConstant::Compile(context, static_cast<const CHIR::Constant&>(expression));
            break;
        case CHIR::ExprKind::NUMERIC_CAST:
            CompileNumericCast::Compile(context, static_cast<const CHIR::NumericCast&>(expression));
            break;
        case CHIR::ExprKind::TRY_NUMERIC_CAST: {
            const auto& cast = static_cast<const CHIR::TryNumericCast&>(expression);
            if (!cast.GetSuccessBlock() || !cast.GetErrorBlock()) {
                context.Fail(JitFailureKind::MissingBranchDestination);
                break;
            }
            CompileNumericCast::Compile(context, cast, cast.GetSuccessBlock(), cast.GetErrorBlock());
            break;
        }
        case CHIR::ExprKind::DEBUGEXPR:
            CompileDebug::Compile(context, static_cast<const CHIR::Debug&>(expression));
            break;
        case CHIR::ExprKind::ALLOCATE: {
            const auto& allocation = static_cast<const CHIR::Allocate&>(expression);
            if (allocation.GetType() && allocation.GetType()->IsClass()) {
                CompileObjectAllocate(context, allocation);
            } else {
                CompileAllocate(context, allocation);
            }
            break;
        }
        case CHIR::ExprKind::TRY_ALLOCATE:
            CompileObjectAllocate(context, static_cast<const CHIR::TryAllocate&>(expression));
            break;
        case CHIR::ExprKind::START_REGION:
            CompileLocalRegion(context, static_cast<const CHIR::StartRegion&>(expression));
            break;
        case CHIR::ExprKind::END_REGION:
            CompileLocalRegion(context, static_cast<const CHIR::EndRegion&>(expression));
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
        case CHIR::ExprKind::TRY_NEG: {
            const auto& unary = static_cast<const CHIR::TryUnaryExpression&>(expression);
            if (!unary.GetSuccessBlock() || !unary.GetErrorBlock()) {
                context.Fail(JitFailureKind::MissingBranchDestination);
                break;
            }
            CompileUnaryExpression::Compile(context, unary, unary.GetSuccessBlock(), unary.GetErrorBlock());
            break;
        }
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
        case CHIR::ExprKind::TRY_ADD:
        case CHIR::ExprKind::TRY_SUB:
        case CHIR::ExprKind::TRY_MUL:
        case CHIR::ExprKind::TRY_DIV:
        case CHIR::ExprKind::TRY_LSHIFT:
        case CHIR::ExprKind::TRY_RSHIFT:
        case CHIR::ExprKind::TRY_MOD: {
            const auto& binary = static_cast<const CHIR::TryBinaryExpression&>(expression);
            if (!binary.GetSuccessBlock() || !binary.GetErrorBlock()) {
                context.Fail(JitFailureKind::MissingBranchDestination);
                break;
            }
            CompileBinaryExpression::Compile(context, binary, binary.GetSuccessBlock(), binary.GetErrorBlock());
            break;
        }
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
