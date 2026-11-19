// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "FunctionRegistry.h"

#include <utility>

namespace Cangjie::JIT::X86_64 {
FunctionRecord& FunctionRegistry::GetOrCreate(const CHIR::Function& function)
{
    const auto found = records.find(&function);
    if (found != records.end()) {
        return *found->second;
    }
    auto record = std::make_unique<FunctionRecord>(function);
    auto& result = *record;
    records.emplace(&function, std::move(record));
    return result;
}
} // namespace Cangjie::JIT::X86_64
