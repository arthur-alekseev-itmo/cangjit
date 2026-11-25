// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

// Name, type, exported symbol, required at initialization.
// Initializes the Cangjie runtime.
JIT_RUNTIME_FUNCTION(InitRuntime, decltype(&::InitCJRuntime), "InitCJRuntime", true)
// Finalizes the Cangjie runtime.
JIT_RUNTIME_FUNCTION(FiniRuntime, decltype(&::FiniCJRuntime), "FiniCJRuntime", true)
// Loads a Cangjie library and initializes it.
JIT_RUNTIME_FUNCTION(LoadLibrary, decltype(&::LoadCJLibraryWithInit), "LoadCJLibraryWithInit", true)
// Unloads a Cangjie library.
JIT_RUNTIME_FUNCTION(UnloadLibrary, decltype(&::UnloadCJLibrary), "UnloadCJLibrary", true)
// Finds a Cangjie symbol by name.
JIT_RUNTIME_FUNCTION(FindSymbol, decltype(&::FindCJSymbol), "FindCJSymbol", true)
// Runs a Cangjie task.
JIT_RUNTIME_FUNCTION(RunTask, decltype(&::RunCJTask), "RunCJTask", true)
// Retrieves the return value of a task.
JIT_RUNTIME_FUNCTION(TaskReturn, decltype(&::GetTaskRet), "GetTaskRet", true)
// Releases a task handle.
JIT_RUNTIME_FUNCTION(ReleaseTask, decltype(&::ReleaseHandle), "ReleaseHandle", true)
// Provides the native-to-Cangjie call stub.
JIT_RUNTIME_FUNCTION(NativeToCangjieStub, void*, "CJ_MCC_N2CStub", true)
// Throws an exception through the runtime.
JIT_RUNTIME_FUNCTION(ThrowException, void*, "CJ_MCC_ThrowException", true)
// Retrieves the runtime exception wrapper.
JIT_RUNTIME_FUNCTION(ExceptionWrapper, void*, "CJ_MCC_GetExceptionWrapper", true)
// Performs runtime work after an exception is thrown.
JIT_RUNTIME_FUNCTION(PostThrowException, void*, "CJ_MCC_PostThrowException", true)
// Provides the Cangjie-to-native call stub.
JIT_RUNTIME_FUNCTION(CangjieToNativeStub, void*, "CJ_MCC_C2NStub", false)
// Looks up type information by mangled name.
JIT_RUNTIME_FUNCTION(TypeByName, void*, "CJ_MCC_GetTypeByMangledName", false)
// Retrieves or creates type information.
JIT_RUNTIME_FUNCTION(CreateTypeInfo, void*, "CJ_MCC_GetOrCreateTypeInfo", false)
// Removes a local finalizer.
JIT_RUNTIME_FUNCTION(RemoveLocalFinalizer, void*, "CJ_MCC_RemoveLocalFinalizer", false)
// Retrieves the outer type information for a method.
JIT_RUNTIME_FUNCTION(MethodOuterTypeInfo, void*, "CJ_MCC_GetMethodOuterTI", false)
// Retrieves the method table for a type.
JIT_RUNTIME_FUNCTION(MethodTable, void*, "CJ_MCC_GetMTable", false)
// Updates a type's virtual method table.
JIT_RUNTIME_FUNCTION(UpdateVTable, void*, "CJ_MCC_UpdateVMT", false)
// Checks whether one type is a subtype of another.
JIT_RUNTIME_FUNCTION(IsSubType, void*, "CJ_MCC_IsSubType", false)
// Checks whether a type is a tuple type.
JIT_RUNTIME_FUNCTION(IsTupleTypeOf, void*, "CJ_MCC_IsTupleTypeOf", false)
// Allocates an object in the current local region.
JIT_RUNTIME_FUNCTION(NewLocalObject, void*, "CJ_MCC_NewLocalObject", false)
// Creates a finalizer.
JIT_RUNTIME_FUNCTION(NewFinalizer, void*, "CJ_MCC_NewFinalizer", false)
// Allocates a weak reference object in the current local region.
JIT_RUNTIME_FUNCTION(NewLocalWeakRefObject, void*, "CJ_MCC_NewLocalWeakRefObject", false)
// Allocates a weak reference object.
JIT_RUNTIME_FUNCTION(NewWeakRefObject, void*, "CJ_MCC_NewWeakRefObject", false)
// Allocates a pinned object.
JIT_RUNTIME_FUNCTION(NewPinnedObject, void*, "CJ_MCC_NewPinnedObject", false)
// Allocates an object.
JIT_RUNTIME_FUNCTION(NewObject, void*, "CJ_MCC_NewObject", false)
// Adds a finalizer to a local object.
JIT_RUNTIME_FUNCTION(AddLocalFinalizer, void*, "CJ_MCC_AddLocalFinalizer", false)
// Starts a local allocation region.
JIT_RUNTIME_FUNCTION(StartLocalRegion, void*, "CJ_MCC_StartLocalRegion", false)
// Ends a local allocation region.
JIT_RUNTIME_FUNCTION(EndLocalRegion, void*, "CJ_MCC_EndLocalRegion", false)
// Retrieves the instance size from type information.
JIT_RUNTIME_FUNCTION(TypeInstanceSize, void*, "_ZNK12MapleRuntime8TypeInfo15GetInstanceSizeEv", false)
// Creates an overflow exception with a message.
JIT_RUNTIME_FUNCTION(CreateOverflowException, void*, "rt$CreateOverflowException_msg", false)
// Creates an arithmetic exception with a message.
JIT_RUNTIME_FUNCTION(CreateArithmeticException, void*, "rt$CreateArithmeticException_msg", false)
