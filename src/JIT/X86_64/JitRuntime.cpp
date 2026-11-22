// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "JitRuntime.h"

#include <cstdio>
#include <exception>
#include <utility>

namespace Cangjie::JIT::X86_64 {
JitRuntime::JitRuntime(bool stackGrowth) noexcept : stringLiterals(bindings), stackGrowthEnabled(stackGrowth) {}

Result<std::unique_ptr<JitRuntime>, RuntimeError> JitRuntime::Create(const RuntimeConfig& config)
{
    auto runtime = std::unique_ptr<JitRuntime>(new JitRuntime(config.stackGrowth));
    auto result = runtime->bindings.Initialize(config);
    if (!result) {
        auto error = std::move(result).Error();
        if (runtime->bindings.HasFailedClose()) {
            error.failedRuntime = std::move(runtime);
        }
        return Result<std::unique_ptr<JitRuntime>, RuntimeError>::Failure(std::move(error));
    }
    return Result<std::unique_ptr<JitRuntime>, RuntimeError>::Success(std::move(runtime));
}

JitRuntime::~JitRuntime()
{
    CloseOrTerminate();
}

Result<void, RuntimeError> JitRuntime::Close()
{
    return bindings.Close();
}

void JitRuntime::CloseOrTerminate() noexcept
{
    if (bindings.HasFailedClose()) {
        std::fputs("JIT runtime close previously failed; refusing implicit retry\n", stderr);
        std::terminate();
    }
    try {
        auto result = Close();
        if (result) {
            return;
        }
        const auto& error = result.Error();
        std::fprintf(stderr, "JIT runtime close failed: %s", GetRuntimeErrorMessage(error.kind));
        if (!error.detail.empty()) {
            std::fprintf(stderr, ": %s", error.detail.c_str());
        }
        if (error.code != 0) {
            std::fprintf(stderr, " (%d)", error.code);
        }
        std::fputc('\n', stderr);
    } catch (...) {
        std::fputs("JIT runtime close failed unexpectedly\n", stderr);
    }
    std::terminate();
}
} // namespace Cangjie::JIT::X86_64
