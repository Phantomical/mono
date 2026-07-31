# The compiler's positive test suite: ~2700 programs that must compile, run and
# produce IL of the expected size.
#
# compiler-tester runs its cases sequentially in one process, globbed out of
# the working directory, so the corpus is dealt into shard directories by
# cmake/shard-corpus.py and each shard is its own CTest test: run-00/ gets one
# compiler-tester, run-01/ the next, and ctest runs them concurrently.  Cases
# that share a library or a source file are kept in one shard; the script's
# docstring has the details.
set(_bin "${CMAKE_CURRENT_BINARY_DIR}")
set(_sup "${_bin}/support")
set(_src "${CMAKE_CURRENT_SOURCE_DIR}")
set(_shards 64)
mono_profile_dir(_pdir net_4_x)

# ilasm and csc write straight to their output paths, so the support tree has
# to exist before the first of them runs.
file(MAKE_DIRECTORY "${_sup}/dlls")

# Re-globbed so a corpus change re-runs the shard assignment's dependency list;
# the assignment itself is a build step and re-runs when file contents change.
file(GLOB _corpus CONFIGURE_DEPENDS "${_src}/*.cs" "${_src}/*.il")
file(GLOB_RECURSE _dlls "${_src}/dlls/*")

add_custom_command(
  OUTPUT "${_bin}/shards.txt"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/cmake/shard-corpus.py"
          --src "${_src}" --dest "${_bin}" --mode pos --shards ${_shards}
  DEPENDS "${CMAKE_SOURCE_DIR}/cmake/shard-corpus.py" ${_corpus} ${_dlls}
  COMMENT "Sharding mcs/tests"
  VERBATIM)

# -- the support assemblies the corpus references ----------------------------
set(_support "${_bin}/shards.txt")

# One per `*-lib.il` beside the tests, plus the one under dlls/.
file(GLOB _il RELATIVE "${_src}" "${_src}/*-lib.il")
list(APPEND _il dlls/test-883.il)
foreach(_f IN LISTS _il)
  string(REGEX REPLACE "\\.il$" ".dll" _out "${_f}")
  string(REGEX REPLACE "[/.]" "-" _tgt "${_out}")
  mono_add_il_module(
    TARGET  mcs-tests-${_tgt}
    OUTPUT  "${_sup}/${_out}"
    SOURCES "${_src}/${_f}"
    PROFILE net_4_x
    FLAGS   /dll)
  list(APPEND _support "${_sup}/${_out}")
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
    list(APPEND _refs "-r:${_sup}/${_r}")
    list(APPEND _deps "${_sup}/${_r}")
  endforeach()
  set(_sign "")
  if(L_SIGNED)
    set(_sign "-keyfile:${_src}/key.snk" -publicsign)
  endif()
  get_filename_component(_dir "${_sup}/${_out}" DIRECTORY)
  add_custom_command(
    OUTPUT "${_sup}/${_out}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_dir}"
    COMMAND ${_cscmd} -noconfig -nologo -t:library
            "-r:${_pdir}/mscorlib.dll" ${_sign} ${_refs}
            "-out:${_sup}/${_out}" "${_src}/${name}"
    DEPENDS "${_src}/${name}" ${_deps} mcs-net_4_x-corlib ${_rt}
    COMMENT "CSC [net_4_x] ${_out}"
    VERBATIM)
  add_custom_target(mcs-tests-${_tgt} DEPENDS "${_sup}/${_out}")
  set(_support ${_support} "${_sup}/${_out}" PARENT_SCOPE)
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

add_custom_target(mcs-compiler-tests ALL DEPENDS ${_support})
add_dependencies(mcs-compiler-tests mcs-net_4_x-mcs mcs-net_4_x-compiler-tester)

# -issues lists the cases already known to fail, so only a new regression makes
# the run fail.
math(EXPR _last "${_shards} - 1")
foreach(_i RANGE ${_last})
  string(REGEX REPLACE "^(.)$" "0\\1" _nn "${_i}")
  add_test(NAME compiler-tests/${_nn}
           COMMAND "${CMAKE_BINARY_DIR}/runtime/mono-wrapper" --debug --verify-all
                   "${_pdir}/compiler-tester.exe"
                   -mode:pos -files:v4
                   "-compiler:${_pdir}/mcs.exe"
                   "-reference-dir:${_pdir}"
                   "-issues:${_src}/known-issues-net_4_x"
                   "-log:${_bin}/net_4_x-${_nn}.log"
                   "-il:${_src}/ver-il-net_4_x.xml"
                   "-compiler-options:-d:NET_4_0;NET_4_5 -debug"
           WORKING_DIRECTORY "${_bin}/run-${_nn}")
  set_tests_properties(compiler-tests/${_nn} PROPERTIES
    LABELS "compiler"
    TIMEOUT 1800
    # The shard directory is on MONO_PATH because compiler-tester compiles the
    # `*-lib.cs` cases into it and the tests that use them load them by name.
    ENVIRONMENT "MONO_PATH=${_pdir}:${_bin}/run-${_nn};MONO_RUNTIME=${CMAKE_BINARY_DIR}/runtime/mono-wrapper")
endforeach()
