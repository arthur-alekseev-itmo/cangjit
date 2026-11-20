// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_JIT_RUNTIME_BINDINGS_H
#define CANGJIE_JIT_X86_64_JIT_RUNTIME_BINDINGS_H

#include <functional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <variant>
#include <vector>

struct RuntimeParam;

namespace Cangjie::JIT::X86_64 {
// Runtime path, native library paths, runtime parameters, owner mode,
// stack-growth mode, sidecar path, and callback to bind sidecar entry slots.
// The host guarantees runtime ownership, stack-growth mode, and that loaded
// libraries are not owned by another client.
using BindingResult = std::string; // empty on success, otherwise a diagnostic
using RuntimeConfig = std::tuple<std::string, std::vector<std::string>, const ::RuntimeParam*, bool, bool,
    std::string, std::function<BindingResult()>>;
using SymbolName = std::string_view;
using SymbolResult = std::variant<void*, std::string>;

class JitRuntimeBindings {
public:
    JitRuntimeBindings() = default;
    JitRuntimeBindings(const JitRuntimeBindings&) = delete;
    JitRuntimeBindings& operator=(const JitRuntimeBindings&) = delete;

    BindingResult Initialize(const RuntimeConfig& config);
    SymbolResult Resolve(SymbolName name);
    // The host must first quiesce generated code and release all objects and
    // tasks referencing the loaded libraries. Calls are externally serialized.
    void Shutdown();

private:
    void* runtime = nullptr;
    std::vector<std::string> libraries;
    std::unordered_map<std::string, void*> symbols;
    bool ownsRuntime = false;
    bool initialized = false;
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_JIT_RUNTIME_BINDINGS_H
