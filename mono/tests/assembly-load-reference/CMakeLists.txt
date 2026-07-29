# Assembly.LoadFrom / Assembly.LoadFile resolution, checked in three layouts:
# the dependency next to the caller, in a directory of its own, and split
# between the two. The same two programs are compiled into each layout; what
# differs is where their dependencies sit on disk.

set(_alr_src "${CMAKE_CURRENT_SOURCE_DIR}")
set(_alr_bin "${CMAKE_CURRENT_BINARY_DIR}")

set(_alr_outputs "")

function(_alr_library out source)
  cmake_parse_arguments(ARG "" "" "REFS" ${ARGN})
  # A reference built here is also a build dependency, or a parallel build
  # compiles against a file that does not exist yet.
  set(_refs "")
  set(_refdeps "")
  foreach(_r IN LISTS ARG_REFS)
    list(APPEND _refs "-r:${_alr_bin}/${_r}")
    list(APPEND _refdeps "${_alr_bin}/${_r}")
  endforeach()
  get_filename_component(_d "${_alr_bin}/${out}" DIRECTORY)
  add_custom_command(
    OUTPUT "${_alr_bin}/${out}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_d}"
    COMMAND ${_csc} -target:library "-out:${_alr_bin}/${out}" "${_alr_src}/${source}" ${_refs}
    DEPENDS "${_alr_src}/${source}" ${_refdeps} mono-test-toolchain
    COMMENT "CSC assembly-load-reference/${out}"
    VERBATIM)
  set(_alr_outputs "${_alr_outputs};${_alr_bin}/${out}" PARENT_SCOPE)
endfunction()

_alr_library(src/Dep.dll src/Dep.cs)
_alr_library(src/Mid.dll src/Mid.cs REFS src/Dep.dll)

# The same two libraries, copied into each layout.
foreach(_place "samedir/Mid.dll:src/Mid.dll" "samedir/Dep.dll:src/Dep.dll"
               "separatedir/middle/Mid.dll:src/Mid.dll"
               "separatedir/middle/Dep.dll:src/Dep.dll"
               "mainanddep/middle/Mid.dll:src/Mid.dll"
               "mainanddep/Dep.dll:src/Dep.dll")
  string(REPLACE ":" ";" _p "${_place}")
  list(GET _p 0 _dst)
  list(GET _p 1 _srcdll)
  get_filename_component(_d "${_alr_bin}/${_dst}" DIRECTORY)
  add_custom_command(
    OUTPUT "${_alr_bin}/${_dst}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_d}"
    COMMAND "${CMAKE_COMMAND}" -E copy "${_alr_bin}/${_srcdll}" "${_alr_bin}/${_dst}"
    DEPENDS "${_alr_bin}/${_srcdll}"
    VERBATIM)
  list(APPEND _alr_outputs "${_alr_bin}/${_dst}")
endforeach()

set(_alr_tests "")
foreach(_layout samedir separatedir mainanddep)
  foreach(_prog LoadFromMain LoadFileMain)
    add_custom_command(
      OUTPUT "${_alr_bin}/${_layout}/${_prog}.exe"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${_alr_bin}/${_layout}"
      COMMAND ${_csc} -target:exe "-out:${_alr_bin}/${_layout}/${_prog}.exe"
              "${_alr_src}/${_layout}/${_prog}.cs"
      DEPENDS "${_alr_src}/${_layout}/${_prog}.cs" mono-test-toolchain
      COMMENT "CSC assembly-load-reference/${_layout}/${_prog}.exe"
      VERBATIM)
    list(APPEND _alr_outputs "${_alr_bin}/${_layout}/${_prog}.exe")
    list(APPEND _alr_tests "${_layout}/${_prog}.exe")
  endforeach()
endforeach()

add_custom_target(assembly-load-reference-tests DEPENDS ${_alr_outputs})
add_dependencies(runtime-corpora assembly-load-reference-tests)

# Run from this directory, not mono/tests: which of the three layouts a program
# sits in is the thing under test.
mono_runtime_suite(assembly-load-reference TESTS ${_alr_tests}
                   WORKDIR "${_alr_bin}" TIMEOUT 600)
