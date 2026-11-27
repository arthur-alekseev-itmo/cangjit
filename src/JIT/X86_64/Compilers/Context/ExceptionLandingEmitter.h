// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_EXCEPTION_LANDING_EMITTER_H
#define CANGJIE_JIT_X86_64_EXCEPTION_LANDING_EMITTER_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Cangjie::CHIR { class Block; }

namespace Cangjie::JIT::X86_64 {
class FunctionContext;

class ExceptionLandingEmitter final {
public:
    explicit ExceptionLandingEmitter(FunctionContext& context) noexcept : context(context) {}
    void Emit(const CHIR::Block& block, size_t callStart, size_t callEnd);

private:
    std::vector<uintptr_t> GetExceptionTypes(const CHIR::Block& block);
    FunctionContext& context;
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_EXCEPTION_LANDING_EMITTER_H
