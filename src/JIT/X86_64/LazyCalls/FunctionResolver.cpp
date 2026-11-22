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
        return CompilationResult::Failure({"lazy call entry is not initialized"});
    }
    if (record.GetState() == FunctionState::COMPILING) {
        return CompilationResult::Failure({"recursive materialization of the same function"});
    }
    if (record.GetState() == FunctionState::READY) {
        return CompilationResult::Success(record.GetCompiledTarget());
    }
    if (record.GetState() == FunctionState::FAILED) {
        return CompilationResult::Failure({record.GetError()});
    }
    if (!materialize) {
        return CompilationResult::Failure({"missing function materializer"});
    }
    if (!record.BeginCompilation()) {
        return CompilationResult::Failure({"function cannot be materialized"});
    }

    try {
        auto result = [&]() -> CompilationResult {
            try {
                return materialize(record.GetFunction(), context);
            } catch (const std::exception& error) {
                return CompilationResult::Failure({error.what()});
            } catch (...) {
                return CompilationResult::Failure({"function materializer threw an unknown exception"});
            }
        }();
        return record.CompleteCompilation(std::move(result));
    } catch (...) {
        record.FailCompilation();
        throw;
    }
}
} // namespace Cangjie::JIT::X86_64
