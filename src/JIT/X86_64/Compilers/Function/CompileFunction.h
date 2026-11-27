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
class Parameter;
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

    CodeAddress CompileToAddress() const;
    void Compile(FunctionContext& context) const;

private:
    CodeAddress CompileToAddressImpl() const;
    CodeAddress Publish(FunctionContext& context) const;

    CodeAddress AllocateCode(FunctionContext& context) const;
    void PrintDebugInfo(const FunctionContext& context) const;
    void MakePrologue(FunctionContext& context) const;
    void MakeEpilogue(FunctionContext& context, const ParameterPlacement& placement) const;
    void SaveReturnValue(FunctionContext& context, const ParameterPlacement& placement) const;
    void SaveParameters(FunctionContext& context, const ParameterPlacement& placements) const;
    void SaveParameter(FunctionContext& context, const ParameterPlacement& placements,
        const CHIR::Parameter* parameter) const;
    void SaveHiddenTypeInfo(FunctionContext& context, const ParameterPlacement& placements) const;
    void SaveHiddenTypeInfoLocation(FunctionContext& context, const Location& location, size_t index) const;

    GlobalContext& globalContext;
    const CHIR::Function& function;
};

} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_COMPILERS_COMPILE_FUNCTION_H
