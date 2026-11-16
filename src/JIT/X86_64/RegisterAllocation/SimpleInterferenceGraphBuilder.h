// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_REGISTER_ALLOCATION_SIMPLE_INTERFERENCE_GRAPH_BUILDER_H
#define CANGJIE_JIT_X86_64_REGISTER_ALLOCATION_SIMPLE_INTERFERENCE_GRAPH_BUILDER_H

#include "InterferenceGraphBuilder.h"

namespace Cangjie::JIT::X86_64 {

class SimpleInterferenceGraphBuilder final : public InterferenceGraphBuilder {
public:
    InterferenceGraph Build(const CHIR::Function& function, const ValueNumeration& numeration) const override;

private:
    static InterferenceGraph BuildComplete(InterferenceGraph::NodeId nodeCount);
};

} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_REGISTER_ALLOCATION_SIMPLE_INTERFERENCE_GRAPH_BUILDER_H
