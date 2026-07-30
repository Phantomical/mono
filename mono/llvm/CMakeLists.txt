# The LLVM JIT.

file(GLOB sources CONFIGURE_DEPENDS
    *.cpp
    *.hpp
    method-to-llvm/*.cpp
    method-to-llvm/*.hpp
)

mono_add_object_library(mono-llvm
    SOURCES ${sources}
    INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../mini"
        "${CMAKE_CURRENT_BINARY_DIR}/../mini"
        "${CMAKE_SOURCE_DIR}/external/bdwgc/include"
        "${CMAKE_SOURCE_DIR}/external/bdwgc/libatomic_ops/src"
    DEPENDS mono::eglib_headers mono::hidden mono::llvm
)

add_dependencies(mono-llvm mini)
