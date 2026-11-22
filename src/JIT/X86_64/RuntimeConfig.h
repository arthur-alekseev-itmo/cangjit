// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_RUNTIME_CONFIG_H
#define CANGJIE_JIT_X86_64_RUNTIME_CONFIG_H

#include <string>
#include <vector>

struct RuntimeParam;

namespace Cangjie::JIT::X86_64 {
// The host guarantees runtime ownership, stack-growth mode, and that loaded
// libraries are not owned by another client.
struct RuntimeConfig {
    std::string runtimePath;
    std::vector<std::string> requestedLibraries;
    const ::RuntimeParam* params;
    bool owner;
    bool stackGrowth;
    std::vector<std::string> importPaths;
    std::vector<std::string> librarySearchPaths;
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_RUNTIME_CONFIG_H
