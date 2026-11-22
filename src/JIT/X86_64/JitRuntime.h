// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_JIT_RUNTIME_H
#define CANGJIE_JIT_X86_64_JIT_RUNTIME_H

#include <memory>

#include "JitRuntimeBindings.h"
#include "Compilers/Context/StringLiteralPool.h"

namespace Cangjie::JIT::X86_64 {
class GlobalContext;

class JitRuntime final {
public:
    static Result<std::unique_ptr<JitRuntime>, RuntimeError> Create(const RuntimeConfig& config);
    ~JitRuntime();

    JitRuntimeBindings& GetBindings() noexcept { return bindings; }
    StringLiteralPool& GetStringLiteralPool() noexcept { return stringLiterals; }
    bool IsStackGrowthEnabled() const noexcept { return stackGrowthEnabled; }
    Result<void, RuntimeError> Close();

private:
    explicit JitRuntime(bool stackGrowth) noexcept;
    void CloseOrTerminate() noexcept;
    friend class GlobalContext;

    JitRuntimeBindings bindings;
    StringLiteralPool stringLiterals;
    bool stackGrowthEnabled;
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_JIT_RUNTIME_H
