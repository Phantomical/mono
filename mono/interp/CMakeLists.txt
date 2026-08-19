
file(GLOB sources CONFIGURE_DEPENDS
  *.cpp
  opcodes/*.cpp
  runtime/*.cpp
  transform/*.cpp
)

mono_add_object_library(mono_ee_interp
    SOURCES ${sources}
    INCLUDES
      "${CMAKE_CURRENT_SOURCE_DIR}"
      "${CMAKE_CURRENT_BINARY_DIR}"
      "${CMAKE_SOURCE_DIR}/external/bdwgc/include"
      "${CMAKE_SOURCE_DIR}/external/bdwgc/libatomic_ops/src"
    DEPENDS mono::hidden mono::eglib_headers mono::llvm)

add_dependencies(mono_ee_interp mono-version)
