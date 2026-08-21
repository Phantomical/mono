# The samples. The autotools build only ever distributed these.  Nothing
# compiled them, so they were free to rot. They are built here because they are
# the only in-tree users of the embedding API and of the profiler module ABI,
# and both break silently otherwise.
#
# They are built, not run and not installed: several want a console or a
# workload, and none of them asserts anything.

if(NOT MONO_ENABLE_SAMPLES OR NOT MONO_ENABLE_LIBRARIES)
  return()
endif()

set(_gc "${MONO_DEFAULT_GC_SUFFIX}")
if(_gc STREQUAL "sgen")
  set(_runtime_lib monosgen-2.0)
else()
  set(_runtime_lib monoboehm-2.0)
endif()

# These are written the way an embedder outside the tree writes them -- against
# the installed layout, where the JIT entry points are <mono/jit/jit.h>. In the
# source tree that header is mono/mini/jit.h and there is no mono/jit/ at all,
# so stage the two public headers into the shape the samples expect. Compiling
# them against the installed spelling is most of the point: it is what catches
# a header that stops being self-contained.
set(_pubinc "${CMAKE_CURRENT_BINARY_DIR}/include")
configure_file("${CMAKE_SOURCE_DIR}/mono/mini/jit.h"
               "${_pubinc}/mono/jit/jit.h" COPYONLY)
configure_file("${CMAKE_SOURCE_DIR}/mono/mini/mono-private-unstable.h"
               "${_pubinc}/mono/jit/mono-private-unstable.h" COPYONLY)

# What `pkg-config --cflags --libs mono-2` resolves to inside the build tree.
function(_mono_sample_embed name)
  add_executable(${name} ${ARGN})
  target_include_directories(${name} PRIVATE
    "${_pubinc}" "${CMAKE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}")
  target_link_libraries(${name} PRIVATE
    ${_runtime_lib} mono::eglib_headers eglib m)
  # Not part of `all`: an embedder that fails to link must not stop a runtime
  # build.  These are only interesting when you work on the API.
  set_target_properties(${name} PROPERTIES
    EXCLUDE_FROM_ALL TRUE
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()

_mono_sample_embed(sample-teste         embed/teste.c)
_mono_sample_embed(sample-test-invoke   embed/test-invoke.c)
_mono_sample_embed(sample-test-metadata embed/test-metadata.c)

# The profiler samples are modules the runtime dlopens, so they are shared
# objects with no main and must not be linked against the runtime.
function(_mono_sample_profiler name source)
  add_library(${name} MODULE "${source}")
  target_include_directories(${name} PRIVATE
    "${_pubinc}" "${CMAKE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}")
  target_link_libraries(${name} PRIVATE mono::eglib_headers)
  set_target_properties(${name} PROPERTIES
    EXCLUDE_FROM_ALL TRUE
    PREFIX "lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endfunction()

_mono_sample_profiler(mono-profiler-sample profiler/sample.c)
_mono_sample_profiler(mono-profiler-size   size/size.c)

# add_dependencies, not DEPENDS: the latter takes files, and these are targets.
add_custom_target(samples)
add_dependencies(samples
  sample-teste sample-test-invoke sample-test-metadata
  mono-profiler-sample mono-profiler-size)

# The managed halves, when there is a class library to compile them against.
if(MONO_ENABLE_MCS_BUILD AND MONO_ENABLE_EXECUTABLES)
  set(_bin      "${CMAKE_CURRENT_BINARY_DIR}")
  set(_buildcls "${MONO_MCS_LIBDIR}/build")
  mono_tools_runtime(_host MONO_PATH "${_buildcls}" AOT_PATH "${_buildcls}")
  set(_csc ${_host} "${MONO_CSC}" -debug:portable -noconfig -nologo)

  add_custom_command(
    OUTPUT "${_bin}/test-embed.exe"
    COMMAND ${_csc} "-out:${_bin}/test-embed.exe"
            "${CMAKE_CURRENT_SOURCE_DIR}/embed/test.cs"
    DEPENDS embed/test.cs mcs
    COMMENT "CSC test-embed.exe"
    VERBATIM)

  # test-invoke.c hardcodes test-embed-invoke-cs.exe as the file it opens from
  # the working directory, not this command's own output name.
  add_custom_command(
    OUTPUT "${_bin}/invoke.exe"
    COMMAND ${_csc} "-out:${_bin}/invoke.exe"
            "${CMAKE_CURRENT_SOURCE_DIR}/embed/invoke.cs"
    DEPENDS embed/invoke.cs mcs
    COMMENT "CSC invoke.exe"
    VERBATIM)

  add_custom_command(
    OUTPUT "${_bin}/objectinspector.dll"
    COMMAND ${_csc} -target:library "-out:${_bin}/objectinspector.dll"
            "${CMAKE_CURRENT_SOURCE_DIR}/size/objectinspector.cs"
    DEPENDS size/objectinspector.cs mcs
    COMMENT "CSC objectinspector.dll"
    VERBATIM)

  add_custom_command(
    OUTPUT "${_bin}/size-sample.exe"
    COMMAND ${_csc} "-r:${_bin}/objectinspector.dll"
            "-out:${_bin}/size-sample.exe"
            "${CMAKE_CURRENT_SOURCE_DIR}/size/sample.cs"
    DEPENDS size/sample.cs "${_bin}/objectinspector.dll" mcs
    COMMENT "CSC size-sample.exe"
    VERBATIM)

  add_custom_target(sample-assemblies
    DEPENDS "${_bin}/test-embed.exe" "${_bin}/invoke.exe"
            "${_bin}/size-sample.exe")
  add_dependencies(samples sample-assemblies)
endif()
