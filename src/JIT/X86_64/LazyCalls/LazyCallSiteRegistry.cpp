// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "LazyCallSiteRegistry.h"

#include "../JitAssert.h"

#include <cstdint>
#include <new>

#include "../../Runtime/RuntimeError.h"

namespace Cangjie::JIT::X86_64 {
void LazyCallSiteRegistry::Register(
    CodeAddress caller, size_t codeSize, const std::vector<PendingLazyCallSite>& pending)
{
    // Entries are produced by EmitCall, which validates the 13-byte
    // lazy-call encoding; insertion cannot fail here.
    try {
        for (const auto& site : pending) {
            JIT_ASSERT(caller != 0 && site.callee != nullptr && site.returnOffset < codeSize &&
                site.immediateOffset <= codeSize && codeSize - site.immediateOffset >= sizeof(uint64_t),
                "invalid pending lazy call site");
            const auto [it, published] =
                sites.emplace(caller + site.returnOffset, LazyCallSite{caller, site.immediateOffset, site.callee});
            JIT_ASSERT(published && it->second.caller == caller &&
                it->second.immediateOffset == site.immediateOffset && it->second.callee == site.callee,
                "lazy call sites must be distinct per caller");
        }
    } catch (const std::bad_alloc&) {
        throw RuntimeError{RuntimeErrorKind::OutOfMemory, 0, "registering lazy call sites"};
    }
}

const LazyCallSite* LazyCallSiteRegistry::Find(CodeAddress returnAddress) const noexcept
{
    const auto it = sites.find(returnAddress);
    return it == sites.end() ? nullptr : &it->second;
}
} // namespace Cangjie::JIT::X86_64
