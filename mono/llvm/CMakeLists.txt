# The LLVM JIT.

file(GLOB sources CONFIGURE_DEPENDS
    *.cpp
    *.hpp
    arch/*.hpp
    arch/amd64/*.cpp
    arch/amd64/*.hpp
    method-to-llvm/*.cpp
    method-to-llvm/*.hpp
    passes/*.cpp
    passes/*.hpp
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

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
