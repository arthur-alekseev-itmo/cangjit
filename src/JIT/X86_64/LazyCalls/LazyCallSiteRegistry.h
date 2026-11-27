// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_LAZY_CALLS_LAZY_CALL_SITE_REGISTRY_H
#define CANGJIE_JIT_X86_64_LAZY_CALLS_LAZY_CALL_SITE_REGISTRY_H

#include <cstddef>
#include <unordered_map>
#include <vector>

#include "../Types.h"

namespace Cangjie::JIT::X86_64 {
class FunctionRecord;

struct PendingLazyCallSite {
    size_t immediateOffset;
    size_t returnOffset;
    FunctionRecord* callee;
};

struct LazyCallSite {
    CodeAddress caller;
    size_t immediateOffset;
    FunctionRecord* callee;
};

class LazyCallSiteRegistry final {
public:
    // Throws RuntimeErrorKind::OutOfMemory when the map allocation fails.
    void Register(CodeAddress caller, size_t codeSize, const std::vector<PendingLazyCallSite>& sites);
    const LazyCallSite* Find(CodeAddress returnAddress) const noexcept;

private:
    std::unordered_map<CodeAddress, LazyCallSite> sites;
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_LAZY_CALLS_LAZY_CALL_SITE_REGISTRY_H
