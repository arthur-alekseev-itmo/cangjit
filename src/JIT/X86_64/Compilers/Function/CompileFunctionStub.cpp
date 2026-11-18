// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "CompileFunctionStub.h"

#include <asmjit/x86.h>

#include <cstdio>
#include <cstdlib>
#include <limits>

#include "../Context/GlobalContext.h"
#include "CompileFunction.h"
#include "cangjie/CHIR/IR/Type/Type.h"
#include "cangjie/CHIR/IR/Value/Value.h"

namespace Cangjie::JIT::X86_64 {
namespace {
using namespace asmjit;
using namespace asmjit::x86;


CompilationResult Materialize(const CHIR::Function& function, void* context)
{
    return CompileFunction(*static_cast<GlobalContext*>(context), function).CompileToAddress();
}

CodeAddress Resolve(GlobalContext* context, FunctionRecord* record) noexcept
{
    try {
        auto result = context->GetFunctionResolver().Resolve(*record, Materialize, context);
        if (const auto* address = std::get_if<CodeAddress>(&result)) {
            return *address;
        }
        std::fprintf(stderr, "JIT lazy compilation failed: %s\n",
            std::get<CompilationError>(result).message.c_str());
    } catch (...) {
        std::fputs("JIT lazy compilation failed with an exception\n", stderr);
    }
    std::abort();
}

constexpr size_t TARGET_ADDRESS_OFFSET = 2;

void Publish(GlobalContext* context, FunctionRecord* record) noexcept
{
    auto* allocator = context->GetRuntime().allocator();
    JitAllocator::Span span;
    const auto entry = reinterpret_cast<void*>(record->GetEntryStub());
    const auto target = static_cast<uint64_t>(record->GetCompiledTarget());
    if (allocator->query(span, entry) != kErrorOk ||
        allocator->write(span, TARGET_ADDRESS_OFFSET, &target, sizeof(target)) != kErrorOk) {
        std::fputs("JIT failed to patch function stub\n", stderr);
        std::abort();
    }
}

bool SupportedSignature(const CHIR::Function& function, size_t& stackArguments)
{
    if (function.IsCFunc() || !function.GetFuncType() || function.GetFuncType()->HasVarArg() ||
        !function.GetReturnType()) {
        return false;
    }
    const auto& returnType = *function.GetReturnType();
    const auto kind = returnType.GetTypeKind();
    if (kind != CHIR::Type::TYPE_INT64 && kind != CHIR::Type::TYPE_UINT64 &&
        !returnType.IsUnit() && !returnType.IsVoid() && !returnType.IsNothing()) {
        return false;
    }
    size_t argumentCount = (returnType.IsUnit() || returnType.IsNothing()) ? 1 : 0;
    for (const auto* parameter : function.GetParams()) {
        if (!parameter || !parameter->GetType() || !parameter->GetType()->IsInteger()) {
            return false;
        }
        ++argumentCount;
    }
    stackArguments = argumentCount > 6 ? argumentCount - 6 : 0;
    return stackArguments <= (static_cast<size_t>(std::numeric_limits<int32_t>::max()) - 15) / 8;
}

class ResolverStubEmitter final {
public:
    explicit ResolverStubEmitter(Assembler& assembler) : a(assembler) {}

    bool Emit(GlobalContext& context, FunctionRecord& record, size_t stackArguments)
    {
        SaveArguments(stackArguments);
        ResolveAndCall(context, record);
        PublishAndReturn(context, record);
        return ok;
    }

private:
    // ok = ok && error == kErrorOk;
    void Emit(Error error)
    {
        ok = ok && error == kErrorOk;
    }

    // savedRegisters = arguments; outgoingStackArgs = incomingStackArgs;
    void SaveArguments(size_t stackArguments)
    {
        Emit(a.push(rbp));
        Emit(a.mov(rbp, rsp));
        Emit(a.push(rdi));
        Emit(a.push(rsi));
        Emit(a.push(rdx));
        Emit(a.push(rcx));
        Emit(a.push(r8));
        Emit(a.push(r9));
        const auto stackBytes = static_cast<int32_t>((stackArguments * 8 + 15) & ~size_t{15});
        if (stackBytes != 0) {
            Emit(a.sub(rsp, stackBytes));
            Emit(a.lea(r11, qword_ptr(rbp, 16)));
            Emit(a.mov(rax, rsp));
            Emit(a.mov(r10, static_cast<uint64_t>(stackArguments)));
            Label copy = a.newLabel();
            Emit(a.bind(copy));
            Emit(a.mov(rdx, qword_ptr(r11)));
            Emit(a.mov(qword_ptr(rax), rdx));
            Emit(a.add(r11, 8));
            Emit(a.add(rax, 8));
            Emit(a.dec(r10));
            Emit(a.jnz(copy));
        }
    }

    // target = Resolve(&context, &record); result = target(savedArguments...);
    void ResolveAndCall(GlobalContext& context, FunctionRecord& record)
    {
        Emit(a.mov(rdi, reinterpret_cast<uintptr_t>(&context)));
        Emit(a.mov(rsi, reinterpret_cast<uintptr_t>(&record)));
        Emit(a.mov(rax, reinterpret_cast<uintptr_t>(&Resolve)));
        Emit(a.call(rax));
        Emit(a.mov(r11, rax));
        Emit(a.mov(rdi, qword_ptr(rbp, -8)));
        Emit(a.mov(rsi, qword_ptr(rbp, -16)));
        Emit(a.mov(rdx, qword_ptr(rbp, -24)));
        Emit(a.mov(rcx, qword_ptr(rbp, -32)));
        Emit(a.mov(r8, qword_ptr(rbp, -40)));
        Emit(a.mov(r9, qword_ptr(rbp, -48)));
        Emit(a.call(r11));
    }

    // savedResult = result; Publish(&context, &record); return savedResult;
    void PublishAndReturn(GlobalContext& context, FunctionRecord& record)
    {
        Emit(a.push(rax));
        Emit(a.sub(rsp, 8));
        Emit(a.mov(rdi, reinterpret_cast<uintptr_t>(&context)));
        Emit(a.mov(rsi, reinterpret_cast<uintptr_t>(&record)));
        Emit(a.mov(rax, reinterpret_cast<uintptr_t>(&Publish)));
        Emit(a.call(rax));
        Emit(a.add(rsp, 8));
        Emit(a.pop(rax));
        Emit(a.mov(rsp, rbp));
        Emit(a.pop(rbp));
        Emit(a.ret());
    }

    Assembler& a;
    bool ok = true;
};

void* MakeResolverStub(GlobalContext& context, FunctionRecord& record, size_t stackArguments)
{
    auto& runtime = context.GetRuntime();
    CodeHolder code;
    if (code.init(runtime.environment()) != kErrorOk) {
        return nullptr;
    }
    Assembler assembler(&code);
    const bool ok = ResolverStubEmitter(assembler).Emit(context, record, stackArguments);
    void* stub = nullptr;
    if (!ok || runtime.add(&stub, &code) != kErrorOk) {
        return nullptr;
    }
    return stub;
}
} // namespace

CodeAddress CompileFunctionStub::Compile(GlobalContext& context, const CHIR::Function* function)
{
#if !defined(__x86_64__) || defined(_WIN32)
    (void)context;
    (void)function;
    return 0;
#else
    size_t stackArguments = 0;
    if (!function || !SupportedSignature(*function, stackArguments)) {
        return 0;
    }
    auto& record = context.GetFunctionRegistry().GetOrCreate(*function);
    if (record.GetEntryStub() != 0) {
        return record.GetEntryStub();
    }

    void* resolverStub = MakeResolverStub(context, record, stackArguments);
    if (!resolverStub) {
        return 0;
    }
    auto& runtime = context.GetRuntime();
    CodeHolder code;
    if (code.init(runtime.environment()) != kErrorOk) {
        runtime.release(resolverStub);
        return 0;
    }
    Assembler assembler(&code);
    const auto targetAddressRegister = r11;
    if (assembler.long_().mov(targetAddressRegister, reinterpret_cast<uintptr_t>(resolverStub)) != kErrorOk ||
        assembler.jmp(targetAddressRegister) != kErrorOk) {
        runtime.release(resolverStub);
        return 0;
    }
    void* entry = nullptr;
    if (runtime.add(&entry, &code) != kErrorOk) {
        runtime.release(resolverStub);
        return 0;
    }
    if (!record.Initialize(reinterpret_cast<CodeAddress>(entry))) {
        runtime.release(entry);
        runtime.release(resolverStub);
        return 0;
    }
    return reinterpret_cast<CodeAddress>(entry);
#endif
}
} // namespace Cangjie::JIT::X86_64
