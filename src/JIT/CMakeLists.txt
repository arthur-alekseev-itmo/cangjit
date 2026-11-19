# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

if(CANGJIE_JIT_X86_64)
    include(FetchContent)
    set(ASMJIT_STATIC TRUE)
    set(ASMJIT_NO_INSTALL TRUE)
    FetchContent_Declare(asmjit
        GIT_REPOSITORY https://github.com/asmjit/asmjit.git
        GIT_TAG 1f8999065baf8ee5e640196fe7186dc62aef12b7)
    FetchContent_MakeAvailable(asmjit)
    set_target_properties(asmjit PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(asmjit PRIVATE -Wno-error=non-virtual-dtor)
    endif()

    file(GLOB JIT_X86_64_SRC CONFIGURE_DEPENDS
        X86_64/*.cpp
        X86_64/Compilers/Function/*.cpp
        X86_64/Compilers/Expression/*.cpp
        X86_64/Compilers/Context/*.cpp
        X86_64/LazyCalls/*.cpp
        X86_64/RegisterAllocation/*.cpp
        X86_64/Utils/*.cpp)
    add_library(CangjieJIT STATIC ${JIT_X86_64_SRC})
    target_include_directories(CangjieJIT SYSTEM PRIVATE ${asmjit_SOURCE_DIR}/src)
    target_compile_options(CangjieJIT PRIVATE ${CJC_EXTRA_WARNINGS})
    target_link_libraries(CangjieJIT PRIVATE asmjit::asmjit)
else()
    add_library(CangjieJIT INTERFACE)
endif()
