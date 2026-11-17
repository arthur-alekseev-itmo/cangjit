// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "FunctionResolver.h"

#include <exception>
#include <utility>

namespace Cangjie::JIT::X86_64 {
CompilationResult FunctionResolver::Resolve(FunctionRecord& record, Materialize materialize, void* context) const
{
    if (record.GetState() == FunctionState::UNINITIALIZED) {
        return CompilationError{"lazy call entry is not initialized"};
    }
    if (record.GetState() == FunctionState::COMPILING) {
        return CompilationError{"recursive materialization of the same function"};
    }
    if (record.GetState() == FunctionState::READY) {
        return record.GetTargetSlot();
    }
    if (record.GetState() == FunctionState::FAILED) {
        return CompilationError{record.GetError()};
    }
    if (!materialize) {
        return CompilationError{"missing function materializer"};
    }
    if (!record.BeginCompilation()) {
        return CompilationError{"function cannot be materialized"};
    }

    CompilationResult result;
    try {
        try {
            result = materialize(record.GetFunction(), context);
        } catch (const std::exception& error) {
            result = CompilationError{error.what()};
        } catch (...) {
            result = CompilationError{"function materializer threw an unknown exception"};
        }
        return record.CompleteCompilation(std::move(result));
    } catch (...) {
        record.FailCompilation();
        throw;
    }
}
} // namespace Cangjie::JIT::X86_64
