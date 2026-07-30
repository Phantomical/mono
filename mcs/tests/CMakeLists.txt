# The compiler's positive test suite: ~2700 programs that must compile, run and
# produce IL of the expected size.
#
# compiler-tester globs its corpus out of the working directory and writes each
# compile's output back into it, so the suite runs from a staging directory in
# the build tree: symlinks to the corpus, real files for everything produced.
set(_run "${CMAKE_CURRENT_BINARY_DIR}/run")
set(_src "${CMAKE_CURRENT_SOURCE_DIR}")
mono_profile_dir(_pdir net_4_x)

file(GLOB _corpus RELATIVE "${_src}"
     "${_src}/*.cs" "${_src}/*.il" "${_src}/*.inc" "${_src}/*.xml"
     "${_src}/*.snk" "${_src}/known-issues-*")
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
set(_support "")

# One per `*-lib.il` beside the tests, plus the one under dlls/.
file(GLOB _il RELATIVE "${_src}" "${_src}/*-lib.il")
list(APPEND _il dlls/test-883.il)
foreach(_f IN LISTS _il)
  string(REGEX REPLACE "\\.il$" ".dll" _out "${_f}")
  string(REGEX REPLACE "[/.]" "-" _tgt "${_out}")
  mono_add_il_module(
    TARGET  mcs-tests-${_tgt}
    OUTPUT  "${_run}/${_out}"
    SOURCES "${_src}/${_f}"
    PROFILE net_4_x
    FLAGS   /dll)
  list(APPEND _support "${_run}/${_out}")
endforeach()

# The C# ones form a small chain -- 939-2 references 939-common, 679-1
# references 679-2 -- and four are signed, because the tests that use them
# check strong-name behaviour.
_mono_csc_command(_csc net_4_x)
_mono_csc_env(_cscenv net_4_x)
_mono_tool_depends(_rt net_4_x)
set(_cscmd ${_csc})
if(_cscenv)
  set(_cscmd "${CMAKE_COMMAND}" -E env ${_cscenv} ${_csc})
endif()

function(_mcs_tests_lib name)
  cmake_parse_arguments(L "SIGNED" "" "REFS" ${ARGN})
  string(REGEX REPLACE "\\.cs$" ".dll" _out "${name}")
  string(REGEX REPLACE "[/.]" "-" _tgt "${_out}")
  set(_refs "")
  set(_deps "")
  foreach(_r IN LISTS L_REFS)
    list(APPEND _refs "-r:${_run}/${_r}")
    list(APPEND _deps "${_run}/${_r}")
  endforeach()
  set(_sign "")
  if(L_SIGNED)
    set(_sign "-keyfile:${_run}/key.snk" -publicsign)
  endif()
  get_filename_component(_dir "${_run}/${_out}" DIRECTORY)
  add_custom_command(
    OUTPUT "${_run}/${_out}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_dir}"
    COMMAND ${_cscmd} -noconfig -nologo -t:library
            "-r:${_pdir}/mscorlib.dll" ${_sign} ${_refs}
            "-out:${_run}/${_out}" "${_src}/${name}"
    DEPENDS "${_src}/${name}" ${_deps} mcs-net_4_x-corlib ${_rt}
    COMMENT "CSC [net_4_x] ${_out}"
    VERBATIM)
  add_custom_target(mcs-tests-${_tgt} DEPENDS "${_run}/${_out}")
  set(_support ${_support} "${_run}/${_out}" PARENT_SCOPE)
endfunction()

_mcs_tests_lib(dlls/test-679-2/test-679-lib-2.cs)
_mcs_tests_lib(dlls/test-679-1/test-679-lib.cs
               REFS dlls/test-679-2/test-679-lib-2.dll)
_mcs_tests_lib(dlls/test-939-common.cs SIGNED)
_mcs_tests_lib(dlls/test-939-1/test-939-lib.cs SIGNED)
_mcs_tests_lib(dlls/test-939-1/test-939-ref.cs SIGNED
               REFS dlls/test-939-1/test-939-lib.dll)
_mcs_tests_lib(dlls/test-939-2/test-939-lib.cs SIGNED
               REFS dlls/test-939-common.dll)

add_custom_target(mcs-compiler-tests DEPENDS ${_support})
add_dependencies(mcs-compiler-tests mcs-net_4_x-mcs mcs-net_4_x-compiler-tester)

add_test(NAME compiler-tests-build
         COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}"
                 --target mcs-compiler-tests)
set_tests_properties(compiler-tests-build PROPERTIES
  LABELS fixture TIMEOUT 1800 FIXTURES_SETUP compiler_tests
  RESOURCE_LOCK ninja)

# -issues lists the cases already known to fail, so only a new regression makes
# the run fail.
add_test(NAME compiler-tests
         COMMAND "${CMAKE_BINARY_DIR}/runtime/mono-wrapper" --debug --verify-all
                 "${_pdir}/compiler-tester.exe"
                 -mode:pos -files:v4
                 "-compiler:${_pdir}/mcs.exe"
                 "-reference-dir:${_pdir}"
                 "-issues:${_run}/known-issues-net_4_x"
                 "-log:${CMAKE_CURRENT_BINARY_DIR}/net_4_x.log"
                 "-il:${_run}/ver-il-net_4_x.xml"
                 "-compiler-options:-d:NET_4_0;NET_4_5 -debug"
         WORKING_DIRECTORY "${_run}")
set_tests_properties(compiler-tests PROPERTIES
  LABELS "compiler"
  TIMEOUT 5400
  FIXTURES_REQUIRED compiler_tests
  # The staging directory is on MONO_PATH because compiler-tester compiles the
  # `*-lib.cs` cases into it and the tests that use them load them by name.
  ENVIRONMENT "MONO_PATH=${_pdir}:${_run};MONO_RUNTIME=${CMAKE_BINARY_DIR}/runtime/mono-wrapper")
