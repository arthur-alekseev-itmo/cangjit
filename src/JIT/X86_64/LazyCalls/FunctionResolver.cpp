// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "FunctionResolver.h"

#include "../JitAssert.h"

namespace Cangjie::JIT::X86_64 {
CodeAddress FunctionResolver::Resolve(FunctionRecord& record, Materialize materialize, void* context) const
{
    JIT_ASSERT(record.GetEntryStub() != 0, "lazy call entry is initialized");
    JIT_ASSERT(record.GetState() != FunctionState::COMPILING,
        "recursive materialization of the same function");
    if (record.GetState() == FunctionState::READY) {
        return record.GetCompiledTarget();
    }
    JIT_ASSERT(materialize != nullptr, "function materializer is provided");
    record.BeginCompilation();
    return record.CompleteCompilation(materialize(record.GetFunction(), context));
}
} // namespace Cangjie::JIT::X86_64
