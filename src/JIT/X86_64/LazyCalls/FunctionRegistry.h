// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_LAZY_CALLS_FUNCTION_REGISTRY_H
#define CANGJIE_JIT_X86_64_LAZY_CALLS_FUNCTION_REGISTRY_H

#include <memory>
#include <unordered_map>

#include "FunctionRecord.h"

namespace Cangjie::JIT::X86_64 {
class FunctionRegistry final {
public:
    FunctionRecord& GetOrCreate(const CHIR::Function& function);

private:
    std::unordered_map<const CHIR::Function*, std::unique_ptr<FunctionRecord>> records;
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_LAZY_CALLS_FUNCTION_REGISTRY_H
