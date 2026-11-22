// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "LazyCallSiteRegistry.h"

#include <cstdint>

namespace Cangjie::JIT::X86_64 {
bool LazyCallSiteRegistry::Register(
    CodeAddress caller, size_t codeSize, const std::vector<PendingLazyCallSite>& pending) noexcept
{
    size_t inserted = 0;
    try {
        for (const auto& site : pending) {
            if (!caller || !site.callee || site.returnOffset >= codeSize ||
                site.immediateOffset > codeSize || codeSize - site.immediateOffset < sizeof(uint64_t) ||
                !sites.emplace(caller + site.returnOffset,
                    LazyCallSite{caller, site.immediateOffset, site.callee}).second) {
                break;
            }
            ++inserted;
        }
    } catch (...) {
        // Roll back the entries installed for this caller.
    }
    if (inserted == pending.size()) {
        return true;
    }
    for (size_t i = 0; i < inserted; ++i) {
        sites.erase(caller + pending[i].returnOffset);
    }
    return false;
}

const LazyCallSite* LazyCallSiteRegistry::Find(CodeAddress returnAddress) const noexcept
{
    const auto it = sites.find(returnAddress);
    return it == sites.end() ? nullptr : &it->second;
}
} // namespace Cangjie::JIT::X86_64
