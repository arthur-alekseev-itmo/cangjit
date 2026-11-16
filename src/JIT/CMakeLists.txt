# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

if(CANGJIE_JIT_X86_64)
    file(GLOB JIT_X86_64_SRC
        X86_64/*.cpp
        X86_64/Compilers/*.cpp
        X86_64/Compilers/Context/*.cpp
        X86_64/RegisterAllocation/*.cpp)
    add_library(CangjieJIT STATIC ${JIT_X86_64_SRC})
    target_compile_options(CangjieJIT PRIVATE ${CJC_EXTRA_WARNINGS})
else()
    add_library(CangjieJIT INTERFACE)
endif()
