// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_X86_64COMPILER_H
#define CANGJIE_JIT_X86_64_X86_64COMPILER_H

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string_view>
#include <string>
#include <vector>

#include "../Runtime/RuntimeConfig.h"
#include "../Runtime/RuntimeError.h"
#include "Types.h"

namespace Cangjie::CHIR {
class Function;
class Package;
}

namespace Cangjie::JIT::X86_64 {
class GlobalContext;
class JitRuntime;

class X86_64Compiler {
public:
    X86_64Compiler();
    X86_64Compiler(std::vector<std::string> importPaths, std::vector<std::string> librarySearchPaths);
    ~X86_64Compiler();

    static Result<std::unique_ptr<X86_64Compiler>, RuntimeError> Create(const RuntimeConfig& config);
    void Run(const CHIR::Package& package, std::ostream& output) const;
    Result<void, RuntimeError> Close() const;

private:
    static const CHIR::Function* FindUserMain(const CHIR::Package& package);
    static bool ValidateUserMain(const CHIR::Function& function, std::ostream& output);
    static bool ValidatePackageInitializers(const CHIR::Package& package, std::ostream& output);
    static bool ConfigureStackGrowth(bool& enabled, std::ostream& output);
    static bool ReportError(std::ostream& output, std::string_view message);

    bool ConfigureRuntime(std::ostream& output) const;
    std::optional<std::uintptr_t> CompileUserMain(const CHIR::Function& function, std::ostream& output) const;
    void RunUserMain(std::uintptr_t address, std::ostream& output) const;

    std::vector<std::string> importPaths;
    std::vector<std::string> librarySearchPaths;
    mutable std::unique_ptr<JitRuntime> jitRuntime;
    mutable std::unique_ptr<GlobalContext> globalContext;
    mutable bool closed = false;
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_X86_64COMPILER_H
