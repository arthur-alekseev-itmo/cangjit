// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_JITCOMPILER_H
#define CANGJIE_JIT_JITCOMPILER_H

#include <iosfwd>

namespace Cangjie::CHIR {
class Package;
}

namespace Cangjie::JIT {
class JITCompiler {
public:
    void Compile(const CHIR::Package& package, std::ostream& output) const;
};
} // namespace Cangjie::JIT

#endif // CANGJIE_JIT_JITCOMPILER_H
