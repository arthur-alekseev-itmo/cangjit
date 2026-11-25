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

    CompilationResult CompileToAddress() const;
    void Compile(FunctionContext& context) const;

private:
    CompilationResult CompileToAddressImpl() const;
    CompilationResult Publish(FunctionContext& context) const;

    bool GetCodeSize(FunctionContext& context, uint32_t& codeSize, CompilationError& error) const;
    bool GetFrameSize(FunctionContext& context, uint32_t& frameSize, CompilationError& error) const;
    bool PrepareMetadata(FunctionContext& context, uint32_t codeSize, uint32_t frameSize, size_t& metadataOffset,
        size_t& metadataSize, CompilationError& error) const;
    CompilationResult AllocateCode(FunctionContext& context) const;
    bool WriteMetadata(FunctionContext& context, CodeAddress base, uint32_t codeSize, size_t metadataOffset,
        size_t metadataSize, size_t& publishedMetadataSize, CompilationError& error) const;
    bool RegisterLazyCallSites(FunctionContext& context, CodeAddress base) const;
    void PrintDebugInfo(const FunctionContext& context) const;
    void MakePrologue(FunctionContext& context) const;
    void MakeEpilogue(FunctionContext& context, const ParameterPlacement& placement) const;
    bool SaveReturnValue(FunctionContext& context, const ParameterPlacement& placement) const;
    bool SaveParameters(FunctionContext& context, const ParameterPlacement& placements) const;
    bool SaveParameter(FunctionContext& context, const ParameterPlacement& placements,
        const CHIR::Parameter* parameter) const;
    bool SaveHiddenTypeInfo(FunctionContext& context, const ParameterPlacement& placements) const;
    bool SaveHiddenTypeInfoLocation(FunctionContext& context, const Location& location, size_t index) const;

    GlobalContext& globalContext;
    const CHIR::Function& function;
};

} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_COMPILERS_COMPILE_FUNCTION_H
