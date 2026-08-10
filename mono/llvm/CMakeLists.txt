# The LLVM JIT.

file(GLOB_RECURSE sources CONFIGURE_DEPENDS
    *.cpp
    *.hpp
)

# The hand-written thunks, named rather than globbed: a sibling arch/ directory
# is a different target's and must not be assembled for this one.
list(APPEND sources
    arch/amd64/interp-entry-thunk.S
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
