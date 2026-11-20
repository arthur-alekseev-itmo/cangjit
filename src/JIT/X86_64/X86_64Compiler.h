// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_X86_64COMPILER_H
#define CANGJIE_JIT_X86_64_X86_64COMPILER_H

#include <iosfwd>
#include <memory>

namespace Cangjie::CHIR {
class Package;
}

namespace Cangjie::JIT::X86_64 {
class GlobalContext;

class X86_64Compiler {
public:
    X86_64Compiler();
    ~X86_64Compiler();

    void Compile(const CHIR::Package& package, std::ostream& output) const;

private:
    std::unique_ptr<GlobalContext> globalContext;
    mutable bool runtimeConfigured = false;
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_X86_64COMPILER_H
