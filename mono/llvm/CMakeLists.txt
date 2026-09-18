# The LLVM JIT.

file(GLOB_RECURSE sources CONFIGURE_DEPENDS
    *.cpp
    *.hpp
)

mono_add_object_library(mono-llvm
    SOURCES ${sources}
    INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../mini"
        "${CMAKE_CURRENT_BINARY_DIR}/../mini"
        "${CMAKE_CURRENT_SOURCE_DIR}/../mini/tier0"
        "${CMAKE_SOURCE_DIR}/external/bdwgc/include"
        "${CMAKE_SOURCE_DIR}/external/bdwgc/libatomic_ops/src"
    DEPENDS mono::eglib_headers mono::hidden mono::llvm
)

add_dependencies(mono-llvm mini)

# backend.cpp calls mono_tier0_compile () behind this, declared in
# mono/mini/tier0/tier0.h.
if(MONO_ENABLE_TIER0_CLASSIC)
  target_compile_definitions(mono-llvm PRIVATE MONO_ENABLE_TIER0_CLASSIC)
endif()
