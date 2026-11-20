// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_GLOBAL_CONTEXT_H
#define CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_GLOBAL_CONTEXT_H

#include <asmjit/core.h>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>


#include "../../JitRuntimeBindings.h"
#include "../../LazyCalls/FunctionRegistry.h"
#include "../../LazyCalls/FunctionResolver.h"
#include "../../LazyCalls/LazyCallSiteRegistry.h"
#include "cangjie/CHIR/IR/CHIRBuilder.h"
#include "cangjie/CHIR/IR/CHIRContext.h"
#include "cangjie/CHIR/IR/Package.h"

namespace Cangjie::JIT::X86_64 {
class GlobalContext final {
public:
    GlobalContext() : allocatorParams(MakeAllocatorParams()), runtime(&allocatorParams) {}
    ~GlobalContext()
    {
        runtimeBindings.Shutdown();
    }

    BindingResult ConfigureRuntime(const RuntimeConfig& config)
    {
        const bool previousStackGrowth = stackGrowthEnabled;
        stackGrowthEnabled = std::get<4>(config);
        auto result = runtimeBindings.Initialize(config);
        if (!result.empty()) {
            stackGrowthEnabled = previousStackGrowth;
        }
        return result;
    }

    JitRuntimeBindings& GetRuntimeBindings() noexcept
    {
        return runtimeBindings;
    }

    bool IsStackGrowthEnabled() const noexcept
    {
        return stackGrowthEnabled;
    }

    CHIR::CHIRBuilder& GetCHIRBuilder() noexcept
    {
        return chirBuilder;
    }

    void SetPackage(const CHIR::Package& value)
    {
        if (package != &value) {
            package = &value;
            subTypes.clear();
            outerTypeInfoCache.clear();
            subTypesInitialized = false;
        }
    }

    bool NeedsOuterTypeInfo(const CHIR::ClassType& type)
    {
        const auto* def = type.GetClassDef();
        if (auto found = outerTypeInfoCache.find(def); found != outerTypeInfoCache.end()) {
            return found->second;
        }
        if (!def->GetType()->GetGenericArgs().empty() || !package) {
            return true;
        }
        if (!subTypesInitialized) {
            for (const auto* child : package->GetAllCustomTypeDef()) {
                for (const auto* parent : child->GetSuperTypesInCurDef()) {
                    subTypes[parent].push_back(child->GetType());
                }
            }
            subTypesInitialized = true;
        }
        bool needed = false;
        if (auto found = subTypes.find(def->GetType()); found != subTypes.end()) {
            for (const auto* child : found->second) {
                if (!child->GetTypeArgs().empty() ||
                    (child->IsClass() && NeedsOuterTypeInfo(*static_cast<const CHIR::ClassType*>(child)))) {
                    needed = true;
                    break;
                }
            }
        }
        outerTypeInfoCache.emplace(def, needed);
        return needed;
    }

    const void* GetStringLiteral(std::string_view message)
    {
        if (message.size() > std::numeric_limits<uint32_t>::max()) {
            return nullptr;
        }
        std::string key(message);
        auto found = stringLiterals.find(key);
        if (found != stringLiterals.end()) {
            return found->second.data();
        }
        auto symbol = runtimeBindings.Resolve("CJ_MCC_GetTypeByMangledName");
        const auto* lookup = std::get_if<void*>(&symbol);
        if (!lookup || !*lookup) {
            return nullptr;
        }
        auto* typeInfo = reinterpret_cast<void* (*)(const char*)>(*lookup)("RawArray<UInt8>");
        if (!typeInfo) {
            return nullptr;
        }
        std::vector<uint64_t> storage(4 + (message.size() + sizeof(uint64_t) - 1) / sizeof(uint64_t));
        auto& words = stringLiterals.emplace(std::move(key), std::move(storage)).first->second;
        // Native literal layout: String {array, start, length}, followed by
        // RawArray {type info, length, bytes}.
        words[0] = reinterpret_cast<uintptr_t>(words.data() + 2);
        words[1] = static_cast<uint64_t>(message.size()) << 32;
        words[2] = reinterpret_cast<uintptr_t>(typeInfo);
        words[3] = message.size();
        std::memcpy(words.data() + 4, message.data(), message.size());
        return words.data();
    }


    asmjit::JitRuntime& GetRuntime() noexcept
    {
        return runtime;
    }

    FunctionRegistry& GetFunctionRegistry() noexcept
    {
        return functionRegistry;
    }

    FunctionResolver& GetFunctionResolver() noexcept
    {
        return functionResolver;
    }
    LazyCallSiteRegistry& GetLazyCallSiteRegistry() noexcept
    {
        return lazyCallSites;
    }


private:
    static asmjit::JitAllocator::CreateParams MakeAllocatorParams() noexcept
    {
        asmjit::JitAllocator::CreateParams params{};
        params.options = asmjit::JitAllocatorOptions::kUseDualMapping;
        return params;
    }

    CHIR::CHIRContext chirContext;
    CHIR::CHIRBuilder chirBuilder{chirContext};
    asmjit::JitAllocator::CreateParams allocatorParams;
    asmjit::JitRuntime runtime;
    FunctionRegistry functionRegistry;
    LazyCallSiteRegistry lazyCallSites;
    FunctionResolver functionResolver;
    JitRuntimeBindings runtimeBindings;
    std::unordered_map<std::string, std::vector<uint64_t>> stringLiterals;
    const CHIR::Package* package = nullptr;
    std::unordered_map<const CHIR::Type*, std::vector<const CHIR::Type*>> subTypes;
    std::unordered_map<const CHIR::ClassDef*, bool> outerTypeInfoCache;
    bool subTypesInitialized = false;
    bool stackGrowthEnabled = false;
};
} // namespace Cangjie::JIT::X86_64

#endif // CANGJIE_JIT_X86_64_COMPILERS_CONTEXT_GLOBAL_CONTEXT_H
