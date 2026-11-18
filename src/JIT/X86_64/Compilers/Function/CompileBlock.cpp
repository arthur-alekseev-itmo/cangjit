// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "CompileBlock.h"

#include "../Expression/CompileBinaryExpression.h"
#include "../Expression/CompileBranch.h"
#include "../Expression/CompileExit.h"
#include "../Expression/CompileGoTo.h"
#include "../Expression/CompileUnaryExpression.h"
#include "../Context/FunctionContext.h"
#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Expression/Terminator.h"
#include "cangjie/CHIR/IR/Value/Value.h"

namespace Cangjie::JIT::X86_64 {
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
