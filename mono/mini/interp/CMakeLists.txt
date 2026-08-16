
file(GLOB_RECURSE sources CONFIGURE_DEPENDS
    *.c
    *.cpp
)
# The whitebox driver is a test program with its own main, built separately by
# MonoMiniTests.cmake.  Linking it here gives every mono binary a second main.
list(REMOVE_ITEM sources "${CMAKE_CURRENT_SOURCE_DIR}/whitebox.c")

mono_add_object_library(mono_ee_interp
    SOURCES ${sources}
    INCLUDES
      "${CMAKE_CURRENT_SOURCE_DIR}"
      "${CMAKE_CURRENT_BINARY_DIR}"
      "${CMAKE_SOURCE_DIR}/external/bdwgc/include"
      "${CMAKE_SOURCE_DIR}/external/bdwgc/libatomic_ops/src"
    DEPENDS mono::hidden mono::eglib_headers mono::llvm)

add_dependencies(mono_ee_interp mono-version)
