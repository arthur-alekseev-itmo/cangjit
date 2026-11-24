// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_REGISTER_ALLOCATION_LOCAL_FINALIZER_FRONTIER_H
#define CANGJIE_JIT_X86_64_REGISTER_ALLOCATION_LOCAL_FINALIZER_FRONTIER_H

#include <string>
#include <vector>

#include "../Types.h"

namespace Cangjie::CHIR {
class AllocateBase;
class Block;
class Expression;
class Function;
}

namespace Cangjie::JIT::X86_64 {
class Liveness;
class ValueNumeration;

// An after-expression site has a non-null afterExpression and null blocks.
// An edge site has a null afterExpression and non-null predecessor and successor.
// Edge sites include exceptional edges and must be emitted only on that particular edge.
struct LocalFinalizerSite {
    const CHIR::Expression* afterExpression;
    const CHIR::Block* predecessor;
    const CHIR::Block* successor;
};
using LocalFinalizerFrontier = Result<std::vector<LocalFinalizerSite>, std::string>;
LocalFinalizerFrontier ComputeLocalFinalizerFrontier(
    const CHIR::Function& function, const CHIR::AllocateBase& allocation, const Liveness& liveness,
    const ValueNumeration& numeration);
}
#endif
