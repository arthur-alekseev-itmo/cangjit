// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "CompileBlock.h"

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
    const auto* base = static_cast<const CHIR::RefType*>(type)->GetBaseType();
    return base && base->IsClass() && base->IsMustLocalRegion();
}

unsigned ScalarWidth(const CHIR::Type* type)
{
    return !type ? 0 : type->IsBoolean() ? 8 : IsLocalClassReference(type) ? 64 : RegisterUtils::IntegerWidth(*type);
}

bool MatchesReference(const CHIR::Value* reference, const CHIR::Type* type)
{
    if (!reference || !reference->GetType() || !reference->GetType()->IsRef() || !type ||
        (!ScalarWidth(type) && !type->IsUnit())) {
        return false;
    }
    const auto* base = static_cast<const CHIR::RefType*>(reference->GetType())->GetBaseType();
    if (IsLocalClassReference(type)) {
        const auto* expr = reference->IsLocalVar() ? static_cast<const CHIR::LocalVar*>(reference)->GetExpr() : nullptr;
        return base == type && expr && expr->GetExprKind() == CHIR::ExprKind::ALLOCATE;
    }
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
    if (type->IsUnit()) {
        context.GetAssembler().xor_(asmjit::x86::eax, asmjit::x86::eax);
        MemoryUtils::StoreValue(context, result, asmjit::x86::rax);
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
    if (type->IsUnit()) {
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
void* ResolveExceptionSymbol(FunctionContext& context, const char* name)
{
    auto symbol = context.GetGlobalContext().GetRuntimeBindings().Resolve(name);
    auto* address = std::get_if<void*>(&symbol);
    if (!address || !*address) {
        context.Fail(JitFailureKind::UnresolvedSymbol);
        return nullptr;
    }
    return *address;
}

void CompileRTTI(FunctionContext& context, const CHIR::GetRTTI& expression)
{
    if (!MemoryUtils::LoadValue(context, expression.GetOperand(), asmjit::x86::rax)) {
        return;
    }
    auto& a = context.GetAssembler();
    a.mov(asmjit::x86::rax, asmjit::x86::qword_ptr(asmjit::x86::rax));
    a.shl(asmjit::x86::rax, 16);
    a.shr(asmjit::x86::rax, 16);
    MemoryUtils::StoreValue(context, expression.GetResult(), asmjit::x86::rax);
}

void CompileRTTI(FunctionContext& context, const CHIR::GetRTTIStatic& expression)
{
    const auto* type = expression.GetRTTIType();
    if (!type) {
        context.Fail(JitFailureKind::UnsupportedExpression, expression.GetExprKind());
        return;
    }
    if (!context.LoadTypeInfo(*type, asmjit::x86::rax)) {
        return;
    }
    MemoryUtils::StoreValue(context, expression.GetResult(), asmjit::x86::rax);
}

void CompileInstanceOf(FunctionContext& context, const CHIR::InstanceOf& expression)
{
    const auto* object = expression.GetObject();
    const auto* objectType = object ? object->GetType() : nullptr;
    const auto* targetType = expression.GetType();
    const auto* result = expression.GetResult();
    if (!objectType || !objectType->IsRef() || !targetType ||
        !result || !result->GetType() || !result->GetType()->IsBoolean()) {
        context.Fail(JitFailureKind::UnsupportedExpression, expression.GetExprKind());
        return;
    }
    const auto* objectClass = static_cast<const CHIR::RefType*>(objectType)->GetBaseType();
    const auto* targetBase = targetType->IsRef()
        ? static_cast<const CHIR::RefType*>(targetType)->GetBaseType() : targetType;
    if (!objectClass || !objectClass->IsClass() || !targetBase) {
        context.Fail(JitFailureKind::UnsupportedExpression, expression.GetExprKind());
        return;
    }
    auto* subtype = ResolveExceptionSymbol(context, "CJ_MCC_IsSubType");
    auto* tupleTest = objectClass->IsAny() ? ResolveExceptionSymbol(context, "CJ_MCC_IsTupleTypeOf") : nullptr;
    if (!subtype || (objectClass->IsAny() && !tupleTest) ||
        !context.LoadTypeInfo(*targetBase, asmjit::x86::rsi)) {
        return;
    }
    if (!MemoryUtils::LoadValue(context, object, asmjit::x86::rdi)) {
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
    MemoryUtils::StoreValue(context, result, asmjit::x86::rax);
}

void CompileGetException(FunctionContext& context, const CHIR::GetException& expression)
{
    auto* target = ResolveExceptionSymbol(context, "CJ_MCC_GetExceptionWrapper");
    if (!target) {
        return;
    }
    auto& a = context.GetAssembler();
    a.long_().mov(asmjit::x86::r11, asmjit::imm(reinterpret_cast<uintptr_t>(target)));
    a.call(asmjit::x86::r11);
    if (context.RecordSafepoint(expression)) {
        MemoryUtils::StoreValue(context, expression.GetResult(), asmjit::x86::rax);
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
    auto* target = ResolveExceptionSymbol(context, "CJ_MCC_PostThrowException");
    if (!target || !MemoryUtils::LoadValue(context, expression.GetOperand(0), asmjit::x86::rdi)) {
        return;
    }
    auto& a = context.GetAssembler();
    a.long_().mov(asmjit::x86::r11, asmjit::imm(reinterpret_cast<uintptr_t>(target)));
    const auto callStart = a.offset();
    a.call(asmjit::x86::r11);
    const auto callEnd = a.offset();
    if (context.RecordSafepoint(expression)) {
        MemoryUtils::StoreValue(context, expression.GetResult(), asmjit::x86::rax);
    }
    if (normal && unwind && !context.GetFailure()) {
        a.jmp(context.GetEdgeLabel(normal));
        context.EmitExceptionLanding(*unwind, callStart, callEnd);
    }
}

void CompileRaiseException(FunctionContext& context, const CHIR::RaiseException& expression)
{
    const auto* value = expression.GetExceptionValue();
    if (!value || !value->GetType() || (!value->GetType()->IsRef() && !value->GetType()->IsGeneric())) {
        context.Fail(JitFailureKind::UnsupportedExpression, expression.GetExprKind());
        return;
    }
    auto* target = ResolveExceptionSymbol(context, "CJ_MCC_ThrowException");
    if (!target || !MemoryUtils::LoadValue(context, value, asmjit::x86::rdi)) {
        return;
    }
    auto& a = context.GetAssembler();
    a.long_().mov(asmjit::x86::r11, asmjit::imm(reinterpret_cast<uintptr_t>(target)));
    const auto callStart = a.offset();
    a.call(asmjit::x86::r11);
    const auto callEnd = a.offset();
    if (!context.RecordSafepoint(expression)) {
        return;
    }
    a.ud2();
    if (const auto* block = expression.GetExceptionBlock()) {
        context.EmitExceptionLanding(*block, callStart, callEnd);
    }
}

} // namespace

void CompileBlock::Compile(FunctionContext& context, const CHIR::Block& block)
{
    context.SetCurrentBlock(block);
    context.GetAssembler().bind(context.GetBlockLabel(&block));
    if (context.GetFailure() || context.GetError() != asmjit::kErrorOk) {
        return;
    }
    const auto expressions = block.GetExpressions();
    for (size_t i = 0; i < expressions.size(); ++i) {
        CompileExpression(context, *expressions[i]);
        if (context.GetFailure() || context.GetError() != asmjit::kErrorOk ||
            (i + 1 < expressions.size() && !context.EmitFinalizersAfter(*expressions[i], *expressions[i + 1]))) {
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
            const auto* source = cast.GetSourceType();
            const auto* target = cast.GetTargetType();
            const auto* sourceBase = source && source->IsRef()
                ? static_cast<const CHIR::RefType*>(source)->GetBaseType() : nullptr;
            const auto* targetBase = target && target->IsRef()
                ? static_cast<const CHIR::RefType*>(target)->GetBaseType() : nullptr;
            if (!sourceBase || !sourceBase->IsClass() || !targetBase || !targetBase->IsClass()) {
                context.Fail(JitFailureKind::UnsupportedExpression, expression.GetExprKind());
                break;
            }
            if (MemoryUtils::LoadValue(context, cast.GetSourceValue(), asmjit::x86::rax)) {
                MemoryUtils::StoreValue(context, cast.GetResult(), asmjit::x86::rax);
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
