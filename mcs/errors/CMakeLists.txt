# The compiler's negative test suite: ~3250 programs that must each produce a
# particular diagnostic.
#
# Staged the same way as mcs/tests: compiler-tester globs the corpus out of the
# working directory and writes its output back into it.
set(_run "${CMAKE_CURRENT_BINARY_DIR}/run")
set(_src "${CMAKE_CURRENT_SOURCE_DIR}")
mono_profile_dir(_pdir net_4_x)

file(GLOB _corpus RELATIVE "${_src}"
     "${_src}/*.cs" "${_src}/*.il" "${_src}/*.inc" "${_src}/*.pub"
     "${_src}/*.snk" "${_src}/*.response" "${_src}/known-issues-*")
file(GLOB_RECURSE _dlls RELATIVE "${_src}" "${_src}/dlls/*")
list(APPEND _corpus ${_dlls})

foreach(_f IN LISTS _corpus)
  get_filename_component(_d "${_run}/${_f}" DIRECTORY)
  file(MAKE_DIRECTORY "${_d}")
  if(NOT EXISTS "${_run}/${_f}")
    file(CREATE_LINK "${_src}/${_f}" "${_run}/${_f}" SYMBOLIC)
  endif()
endforeach()

# -- the support assemblies the corpus references ----------------------------
_mono_csc_command(_csc net_4_x)
_mono_csc_env(_cscenv net_4_x)
_mono_tool_depends(_rt net_4_x)
set(_cscmd ${_csc})
if(_cscenv)
  set(_cscmd "${CMAKE_COMMAND}" -E env ${_cscenv} ${_csc})
endif()

set(_support "")

# name is the source, relative to this directory.  SIGNED and MODULE follow the
# makefile's per-case overrides; everything else takes the pattern rule.
function(_mcs_errors_lib name)
  cmake_parse_arguments(L "SIGNED;MODULE;WARN" "" "REFS" ${ARGN})
  string(REGEX REPLACE "\\.cs$" ".dll" _out "${name}")
  string(REGEX REPLACE "[/.]" "-" _tgt "${_out}")
  set(_extra "")
  if(NOT L_WARN)
    list(APPEND _extra /warn:0)
  endif()
  if(L_MODULE)
    list(APPEND _extra /target:module)
  else()
    list(APPEND _extra /target:library)
  endif()
  if(L_SIGNED)
    list(APPEND _extra /publicsign "/keyfile:${_run}/key.snk")
  endif()
  # A reference is either one of the staged support assemblies or a class
  # library, which is named without a directory.
  set(_deps "")
  foreach(_r IN LISTS L_REFS)
    if(_r MATCHES "/")
      list(APPEND _extra "/r:${_run}/${_r}")
      list(APPEND _deps "${_run}/${_r}")
    else()
      list(APPEND _extra "/r:${_pdir}/${_r}")
      _mono_stem(_rs "${_r}")
      list(APPEND _deps "mcs-net_4_x-${_rs}")
    endif()
  endforeach()
  get_filename_component(_dir "${_run}/${_out}" DIRECTORY)
  add_custom_command(
    OUTPUT "${_run}/${_out}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_dir}"
    COMMAND ${_cscmd} -nologo -noconfig "/r:${_pdir}/mscorlib.dll" ${_extra}
            "/out:${_run}/${_out}" "${_src}/${name}"
    DEPENDS "${_src}/${name}" ${_deps} mcs-net_4_x-corlib ${_rt}
    COMMENT "CSC [net_4_x] ${_out}"
    VERBATIM)
  add_custom_target(mcs-errors-${_tgt} DEPENDS "${_run}/${_out}")
  set(_support ${_support} "${_run}/${_out}" PARENT_SCOPE)
endfunction()

# Everything matching the pattern rules, minus the cases the makefile overrode.
set(_special
    CS0029-26-lib.cs CS0266-25-lib.cs CSFriendAssembly-lib.cs
    CS1701-lib.cs CS1702-lib.cs CS1705-lib.cs)
file(GLOB _libs RELATIVE "${_src}" "${_src}/*-lib.cs" "${_src}/*-module.cs")
file(GLOB _libs_il RELATIVE "${_src}" "${_src}/*-lib.il")
file(GLOB _first RELATIVE "${_src}" "${_src}/dlls/first/*.cs" "${_src}/dlls/second/*.cs")

foreach(_f IN LISTS _libs)
  if(NOT _f IN_LIST _special)
    if(_f MATCHES "-module\\.cs$")
      _mcs_errors_lib("${_f}" MODULE WARN)
    else()
      _mcs_errors_lib("${_f}")
    endif()
  endif()
endforeach()

# The three-way CS170x cases: a first and a second assembly with the same
# identity, and a third compiled against the first, which is what makes the
# compiler complain when a test binds to the wrong one.
#
# CS1704 is the odd one out and has to stay unsigned: its diagnostic is the one
# the compiler reports for a duplicate that could have been told apart by a
# strong name -- "consider removing one of the references or sign the assembly".
foreach(_f IN LISTS _first)
  if(_f MATCHES "CS1704")
    _mcs_errors_lib("${_f}")
  else()
    _mcs_errors_lib("${_f}" SIGNED)
  endif()
endforeach()
foreach(_n 1701 1702 1705)
  _mcs_errors_lib("CS${_n}-lib.cs" REFS "dlls/first/CS${_n}-lib.dll")
endforeach()

_mcs_errors_lib(CS0029-26-lib.cs SIGNED WARN)
_mcs_errors_lib(CS0266-25-lib.cs WARN REFS System.Core.dll)
_mcs_errors_lib(CSFriendAssembly-lib.cs SIGNED)

foreach(_f IN LISTS _libs_il)
  string(REGEX REPLACE "\\.il$" ".dll" _out "${_f}")
  string(REGEX REPLACE "[/.]" "-" _tgt "${_out}")
  mono_add_il_module(
    TARGET  mcs-errors-${_tgt}
    OUTPUT  "${_run}/${_out}"
    SOURCES "${_src}/${_f}"
    PROFILE net_4_x
    FLAGS   /dll)
  list(APPEND _support "${_run}/${_out}")
endforeach()

# CS1703 checks what happens when two assemblies claim the same identity; the
# second one is the framework's own System.dll.
add_custom_command(
  OUTPUT "${_run}/dlls/cs1703-2/System.dll"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_run}/dlls/cs1703-2"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CMAKE_SOURCE_DIR}/external/binary-reference-assemblies/v2.0/System.dll"
          "${_run}/dlls/cs1703-2/System.dll"
  DEPENDS "${CMAKE_SOURCE_DIR}/external/binary-reference-assemblies/v2.0/System.dll"
  VERBATIM)
list(APPEND _support "${_run}/dlls/cs1703-2/System.dll")

add_custom_target(mcs-error-tests DEPENDS ${_support})
add_dependencies(mcs-error-tests mcs-net_4_x-mcs mcs-net_4_x-compiler-tester)

add_test(NAME compiler-error-tests-build
         COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}"
                 --target mcs-error-tests)
set_tests_properties(compiler-error-tests-build PROPERTIES
  LABELS fixture TIMEOUT 1800 FIXTURES_SETUP compiler_error_tests
  RESOURCE_LOCK ninja)

add_test(NAME compiler-error-tests
         COMMAND "${CMAKE_BINARY_DIR}/runtime/mono-wrapper" --debug
                 "${_pdir}/compiler-tester.exe"
                 -mode:neg -files:v4
                 "-compiler:${_pdir}/mcs.exe"
                 "-reference-dir:${_pdir}"
                 "-issues:${_run}/known-issues-net_4_x"
                 "-log:${CMAKE_CURRENT_BINARY_DIR}/net_4_x.log"
                 "-compiler-options:-v --break-on-ice -d:NET_4_0;NET_4_5"
         WORKING_DIRECTORY "${_run}")
set_tests_properties(compiler-error-tests PROPERTIES
  LABELS "compiler"
  TIMEOUT 5400
  FIXTURES_REQUIRED compiler_error_tests
  ENVIRONMENT "MONO_PATH=${_pdir}:${_run};MONO_RUNTIME=${CMAKE_BINARY_DIR}/runtime/mono-wrapper")
