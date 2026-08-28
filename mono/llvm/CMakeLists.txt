# The LLVM JIT.

file(GLOB_RECURSE sources CONFIGURE_DEPENDS
    *.cpp
    *.hpp
)

# The hand-written thunks, named rather than globbed: a sibling arch/ directory
# is a different target's and must not be assembled for this one.  There is one
# per convention, and each is written in the assembler its host's toolchain has.
if(MONO_HOST_WINDOWS)
  # ml64 has no preprocessor, so the layouts the thunks index through are
  # turned into a MASM include beside them.  The .asm reaches these by the
  # binary directory being on ml64's include path, below.
  set(masm_offsets "")
  foreach(_layout dyn-call-offsets interp-entry-offsets)
    add_custom_command(
      OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${_layout}.inc"
      COMMAND ${CMAKE_COMMAND}
              -D "IN=${CMAKE_CURRENT_SOURCE_DIR}/arch/amd64/${_layout}.h"
              -D "OUT=${CMAKE_CURRENT_BINARY_DIR}/${_layout}.inc"
              -D "DEFINES=HOST_WIN32"
              -P "${CMAKE_SOURCE_DIR}/cmake/MonoMasmOffsets.cmake"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/arch/amd64/${_layout}.h"
              "${CMAKE_SOURCE_DIR}/cmake/MonoMasmOffsets.cmake"
      COMMENT "Generating ${_layout}.inc"
      VERBATIM)
    list(APPEND masm_offsets "${CMAKE_CURRENT_BINARY_DIR}/${_layout}.inc")
  endforeach()

  list(APPEND sources
      ${masm_offsets}
      arch/amd64/dyn-call-thunk.asm
      arch/amd64/interp-entry-thunk.asm
  )
else()
  list(APPEND sources
      arch/amd64/dyn-call-thunk.S
      arch/amd64/interp-entry-thunk.S
  )
endif()

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

if(MONO_HOST_WINDOWS)
  # `include <name>.inc` in a .asm is resolved against ml64's include path, and
  # target_include_directories does not reach the assembler.
  set_source_files_properties(
    arch/amd64/dyn-call-thunk.asm arch/amd64/interp-entry-thunk.asm
    PROPERTIES COMPILE_OPTIONS "/I${CMAKE_CURRENT_BINARY_DIR}")
endif()
