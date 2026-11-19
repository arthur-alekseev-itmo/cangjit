// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_LAZY_CALLS_FUNCTION_RESOLVER_H
#define CANGJIE_JIT_X86_64_LAZY_CALLS_FUNCTION_RESOLVER_H

#include "FunctionRecord.h"

namespace Cangjie::JIT::X86_64 {
class FunctionResolver final {
public:
    using Materialize = CompilationResult (*)(const CHIR::Function& function, void* context);

    CompilationResult Resolve(FunctionRecord& record, Materialize materialize, void* context) const;
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_LAZY_CALLS_FUNCTION_RESOLVER_H
