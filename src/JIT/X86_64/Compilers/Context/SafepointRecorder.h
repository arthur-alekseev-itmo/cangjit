// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_SAFEPOINT_RECORDER_H
#define CANGJIE_JIT_X86_64_SAFEPOINT_RECORDER_H

namespace Cangjie::CHIR {
class Expression;
class Value;
}

namespace Cangjie::JIT::X86_64 {
class FunctionContext;

class SafepointRecorder final {
public:
    explicit SafepointRecorder(FunctionContext& context) noexcept : context(context) {}
    void Record(const CHIR::Expression& expression, const CHIR::Value* temporaryRoot = nullptr);

private:
    FunctionContext& context;
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_SAFEPOINT_RECORDER_H
