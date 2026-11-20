// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_COMPILERS_EXPRESSION_COMPILE_OBJECT_H
#define CANGJIE_JIT_X86_64_COMPILERS_EXPRESSION_COMPILE_OBJECT_H

#include "../Context/FunctionContext.h"
#include "cangjie/CHIR/IR/Expression/Expression.h"
#include "cangjie/CHIR/IR/Expression/Terminator.h"

namespace Cangjie::JIT::X86_64 {
void CompileObjectAllocate(FunctionContext& context, const CHIR::Allocate& expression);
void CompileLocalRegion(FunctionContext& context, const CHIR::StartRegion& expression);
void CompileLocalRegion(FunctionContext& context, const CHIR::EndRegion& expression);
void CompileObjectAllocate(FunctionContext& context, const CHIR::TryAllocate& expression);
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_COMPILERS_EXPRESSION_COMPILE_OBJECT_H
