// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_FUNCTION_CONTEXT_H
#define CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_FUNCTION_CONTEXT_H

#include <iosfwd>

#include "../../RegisterAllocation/InterferenceGraph.h"
#include "../../RegisterAllocation/RegisterAllocation.h"
#include "../../RegisterAllocation/ValueNumeration.h"

namespace Cangjie::CHIR {
class Function;
}

namespace Cangjie::JIT::X86_64 {

/// State owned by the compilation of one CHIR function.
/// The function and all values referenced by the numeration must outlive this context.
class FunctionContext final {
public:
    FunctionContext(const CHIR::Function* function, ValueNumeration valueNumeration,
        InterferenceGraph interferenceGraph, RegisterAllocation registerAllocation) noexcept;

    const CHIR::Function* GetFunction() const noexcept;
    const ValueNumeration& GetValueNumeration() const noexcept;
    const InterferenceGraph& GetInterferenceGraph() const noexcept;
    const RegisterAllocation& GetRegisterAllocation() const noexcept;
    void Dump(std::ostream& output) const;

private:
    const CHIR::Function* function;
    ValueNumeration valueNumeration;
    InterferenceGraph interferenceGraph;
    RegisterAllocation registerAllocation;
};

} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_FUNCTION_CONTEXT_H
