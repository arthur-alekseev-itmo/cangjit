// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_REGISTER_ALLOCATION_SAFEPOINT_LIVENESS_H
#define CANGJIE_JIT_X86_64_REGISTER_ALLOCATION_SAFEPOINT_LIVENESS_H

#include "../../Runtime/FunctionMetadata.h"
#include "RegisterAllocation.h"

namespace Cangjie::CHIR {
class Expression;
class Function;
}

namespace Cangjie::JIT::X86_64 {
class Liveness;
// GC roots, stack-interior roots, and derived locations per GC root, in the
// ordering required by FunctionMetadata::RecordSafepoint.
struct SafepointRoots {
    FunctionMetadata::Roots gcRoots;
    FunctionMetadata::Roots stackRoots;
    FunctionMetadata::DerivedRoots derivedRoots;
};

// Analyze immediately before expression: arguments are live; its result is not
// yet initialized.
SafepointRoots ComputeSafepointLiveness(const CHIR::Function& function,
    const ValueNumeration& numeration, const RegisterAllocation& allocation, const Liveness& liveness,
    const CHIR::Expression& expression);
} // namespace Cangjie::JIT::X86_64
#endif
