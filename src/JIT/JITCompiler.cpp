// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "JITCompiler.h"

#include "cangjie/CHIR/Utils/CHIRPrinter.h"

namespace Cangjie::JIT {
void JITCompiler::Compile(const CHIR::Package& package, std::ostream& output) const
{
    CHIR::CHIRPrinter::PrintPackage(package, output);
}
} // namespace Cangjie::JIT
