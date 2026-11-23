// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_COMPILERS_COMPILE_FUNCTION_H
#define CANGJIE_JIT_X86_64_COMPILERS_COMPILE_FUNCTION_H

#include "../../Types.h"

namespace Cangjie::CHIR {
class Function;
class Type;
}

namespace Cangjie::JIT::X86_64 {
class FunctionContext;
class GlobalContext;
class Location;
struct ParameterPlacement;


class CompileFunction final {
public:
    CompileFunction(GlobalContext& globalContext, const CHIR::Function& function) noexcept;

    CompilationResult CompileToAddress() const;
    void Compile(FunctionContext& context) const;

private:
    CompilationResult CompileToAddressImpl() const;
    void PrintDebugInfo(const FunctionContext& context) const;
    void MakePrologue(FunctionContext& context) const;
    void MakeEpilogue(FunctionContext& context) const;
    bool SaveReturnValue(FunctionContext& context) const;
    void LoadReturnValueFromAddress(FunctionContext& context, const CHIR::Type& type, unsigned width) const;
    bool SaveParameters(FunctionContext& context, const ParameterPlacement& placements) const;
    bool SaveHiddenTypeInfo(FunctionContext& context, const ParameterPlacement& placements) const;
    bool SaveHiddenTypeInfoLocation(FunctionContext& context, const Location& location, size_t index) const;

    GlobalContext& globalContext;
    const CHIR::Function& function;
};

} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_COMPILERS_COMPILE_FUNCTION_H
