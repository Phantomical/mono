# The runtime regression corpus: ~700 small managed programs plus the native
# library they P/Invoke into.
#
# The corpus itself lives in tests.cmake; this file is the rules.
#
# Only the JIT modes are here. The AOT ones (fullaot-mixed, llvmonly-mixed, and
# the `%.exe.so` AOT-image rules that shadowed every test) are out of this
# port's scope, so the TESTSAOT_* lists and the AOT variants of each suite are
# absent rather than carried along switched off.

if(NOT MONO_ENABLE_MCS_BUILD OR NOT MONO_ENABLE_EXECUTABLES)
  return()
endif()

include(tests.cmake)

set(_src      "${CMAKE_CURRENT_SOURCE_DIR}")
set(_bin      "${CMAKE_CURRENT_BINARY_DIR}")
set(_class    "${MONO_MCS_LIBDIR}/${MONO_DEFAULT_PROFILE}")
set(_buildcls "${MONO_MCS_LIBDIR}/build")
set(_wrapper  "${CMAKE_BINARY_DIR}/runtime/mono-wrapper")

# csc and ilasm run on whichever mono MONO_USE_SYSTEM_RUNTIME_FOR_TOOLS selected,
# against the build profile of the class libraries; see MonoToolsRuntime.cmake.
mono_tools_runtime(_host MONO_PATH "${_buildcls}" AOT_PATH "${_buildcls}")
set(_csc ${_host} "${MONO_CSC}"
         -debug:portable -noconfig -nologo
         -nowarn:0162 -nowarn:0168 -nowarn:0219 -nowarn:0414 -nowarn:0618
         -nowarn:0169 -nowarn:1690 -nowarn:0649 -nowarn:0612 -nowarn:3021
         -nowarn:0197 -langversion:latest)
set(_csc_unsafe ${_csc} -unsafe)
# /quiet drops the per-file banner and the success line; warnings and errors are
# printed either way.
set(_ilasm ${_host} "${_buildcls}/ilasm.exe" /quiet)

# The reference set an ordinary test gets. Mono.Posix is what TestDriver uses
# to hard-kill a test that hangs past its timeout.
#
# -nostdlib names this tree's mscorlib rather than the compiler host's. The rest
# of this set is already this tree's, so without it a test compiles against a
# System.dll built here and an mscorlib from the machine's mono, and a type this
# tree adds to corlib is invisible to the corpus. It sits here rather than in
# _csc so that NO_DEFAULT_REFS still means what it says: bug-3903 targets the
# v2.0 reference assemblies and brings its own.
set(_default_refs
  -nostdlib
  "-r:${_class}/mscorlib.dll"
  "-r:${_class}/System.dll"
  "-r:${_class}/System.Xml.dll"
  "-r:${_class}/System.Core.dll"
  "-r:${_bin}/TestDriver.dll"
  "-r:${_class}/Mono.Posix.dll")

set(_all_assemblies "")

# ---------------------------------------------------------------------------
# mono_test_assembly(<output> SOURCES ... [IL] [LIBRARY] [MODULE]
#                    [NO_DEFAULT_REFS] [REFS ...] [DEFINES ...]
#                    [FLAGS ...] [DEPENDS ...])
#
# One managed assembly. Everything below is either a call to this or, for the
# handful of tests automake built with a multi-command recipe, a custom command
# of its own.
# ---------------------------------------------------------------------------
function(mono_test_assembly out)
  cmake_parse_arguments(ARG "IL;LIBRARY;MODULE;NO_DEFAULT_REFS" ""
                            "SOURCES;REFS;DEFINES;FLAGS;DEPENDS" ${ARGN})

  set(_srcs "")
  foreach(_s IN LISTS ARG_SOURCES)
    if(IS_ABSOLUTE "${_s}")
      list(APPEND _srcs "${_s}")
    else()
      list(APPEND _srcs "${_src}/${_s}")
    endif()
  endforeach()

  set(_refs "")
  set(_refdeps "")
  foreach(_r IN LISTS ARG_REFS)
    if(_r MATCHES "\\.(dll|exe)$" AND NOT IS_ABSOLUTE "${_r}")
      list(APPEND _refs "-r:${_r}")
      list(APPEND _refdeps "${_bin}/${_r}")
    else()
      list(APPEND _refs "-r:${_r}")
    endif()
  endforeach()

  set(_defs "")
  foreach(_d IN LISTS ARG_DEFINES)
    list(APPEND _defs "-d:${_d}")
  endforeach()

  get_filename_component(_outdir "${_bin}/${out}" DIRECTORY)

  if(ARG_IL)
    if(ARG_LIBRARY)
      set(_kind /dll)
    else()
      set(_kind /exe)
    endif()
    add_custom_command(
      OUTPUT "${_bin}/${out}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${_outdir}"
      COMMAND ${_ilasm} ${_kind} "/output:${_bin}/${out}" ${_srcs}
      DEPENDS ${_srcs} ${_refdeps} ${ARG_DEPENDS} mono-test-toolchain
      WORKING_DIRECTORY "${_bin}"
      COMMENT "ILASM ${out}"
      VERBATIM)
  else()
    if(ARG_LIBRARY)
      set(_kind -target:library)
    elseif(ARG_MODULE)
      set(_kind -target:module)
    else()
      set(_kind "")
    endif()
    if(ARG_NO_DEFAULT_REFS)
      set(_base "")
    else()
      set(_base ${_default_refs})
    endif()
    add_custom_command(
      OUTPUT "${_bin}/${out}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${_outdir}"
      COMMAND ${_csc_unsafe} ${_kind} ${ARG_FLAGS} ${_defs} ${_base} ${_refs}
              "-out:${_bin}/${out}" ${_srcs}
      DEPENDS ${_srcs} ${_refdeps} ${ARG_DEPENDS} mono-test-toolchain
      WORKING_DIRECTORY "${_bin}"
      COMMENT "CSC ${out}"
      VERBATIM)
  endif()

  set(_all_assemblies "${_all_assemblies};${_bin}/${out}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# The toolchain the corpus is compiled with: TestDriver.dll, test-runner.exe
# and libtest.
# ---------------------------------------------------------------------------
add_custom_command(
  OUTPUT "${_bin}/TestDriver.dll"
  COMMAND ${_csc_unsafe} -target:library "-out:${_bin}/TestDriver.dll"
          "${CMAKE_SOURCE_DIR}/mono/mini/TestDriver.cs"
          "${CMAKE_SOURCE_DIR}/mono/mini/TestHelpers.cs"
  DEPENDS "${CMAKE_SOURCE_DIR}/mono/mini/TestDriver.cs"
          "${CMAKE_SOURCE_DIR}/mono/mini/TestHelpers.cs"
  COMMENT "CSC TestDriver.dll"
  VERBATIM)

# test-runner.exe references the *build* profile's framework assemblies, not
# the profile under test: it is the harness, not a test.
#
# CTest no longer drives it -- each corpus program is its own CTest test, see
# runtime-suites.cmake. It stays because running a list of tests by hand,
# without a configure step, is still the quickest way to bisect a corpus.
add_custom_command(
  OUTPUT "${_bin}/test-runner.exe"
  COMMAND ${_csc_unsafe} -r:System.dll -r:System.Xml.dll -r:System.Core.dll
          "-r:${_bin}/TestDriver.dll" "-r:${_class}/Mono.Posix.dll"
          "-out:${_bin}/test-runner.exe" "${_src}/test-runner.cs"
  DEPENDS "${_src}/test-runner.cs" "${_bin}/TestDriver.dll"
  COMMENT "CSC test-runner.exe"
  VERBATIM)

# libtest is the P/Invoke target for the marshalling and cominterop tests. It
# is dlopen'ed by bare name, so no version suffix.
add_library(libtest SHARED libtest.c)
set_target_properties(libtest PROPERTIES
  OUTPUT_NAME test
  LIBRARY_OUTPUT_DIRECTORY "${_bin}")
target_link_libraries(libtest PRIVATE mono::common mono::eglib_headers eglib)
target_include_directories(libtest PRIVATE "${CMAKE_SOURCE_DIR}")
# mono_test_marshal_delegate () makes indirect calls to stdcall functions;
# turning them into tail calls miscompiles on some gcc versions.
target_compile_options(libtest PRIVATE -fno-optimize-sibling-calls)

add_custom_target(mono-test-toolchain
  DEPENDS "${_bin}/TestDriver.dll" "${_bin}/test-runner.exe" libtest)
# Every compile below reaches mono-test-toolchain first, so this one dependency
# orders the whole corpus after the class libraries it compiles against.
add_dependencies(mono-test-toolchain mcs)
mono_tools_runtime_depends(mono-test-toolchain)

# ---------------------------------------------------------------------------
# Generated sources: three array tests are the same body instantiated over a
# big struct, a small struct and an int.
# ---------------------------------------------------------------------------
foreach(_variant bigvt smallvt int)
  # add_custom_command has no output redirection, and the generators write to
  # stdout, so these go through a shell.
  add_custom_command(
    OUTPUT "${_bin}/array-coop-${_variant}.cs"
    COMMAND sh -c "'${_src}/array-coop-${_variant}.sh' '${_src}' > '${_bin}/array-coop-${_variant}.cs'"
    DEPENDS "${_src}/array-coop-${_variant}.sh"
            "${_src}/array-coop-1.cs" "${_src}/array-coop-2.cs"
    COMMENT "GEN array-coop-${_variant}.cs"
    VERBATIM)
endforeach()

# Two more sources are produced by running a generator, which is itself one of
# the test programs.
foreach(_pair "gen-runtime-invoke:runtime-invoke.gen" "make-imt-test:imt_big_iface_test")
  string(REPLACE ":" ";" _p "${_pair}")
  list(GET _p 0 _gen)
  list(GET _p 1 _out)
  add_custom_command(
    OUTPUT "${_bin}/${_gen}.exe"
    COMMAND ${_csc_unsafe} ${_default_refs} "-out:${_bin}/${_gen}.exe" "${_src}/${_gen}.cs"
    DEPENDS "${_src}/${_gen}.cs" mono-test-toolchain
    COMMENT "CSC ${_gen}.exe"
    VERBATIM)
  add_custom_command(
    OUTPUT "${_bin}/${_out}.cs"
    COMMAND sh -c "MONO_PATH='${_class}' '${_wrapper}' '${_bin}/${_gen}.exe' > '${_bin}/${_out}.cs'"
    DEPENDS "${_bin}/${_gen}.exe"
    WORKING_DIRECTORY "${_bin}"
    COMMENT "GEN ${_out}.cs"
    VERBATIM)
endforeach()

# Two tailcall corpora are one large IL file each, split into one test per
# function by a small C++ program. The splitters read stdin and write into the
# working directory, so they run from the build tree with the target directory
# made first.
add_executable(tailcall-split-fsharp tailcall/split-fsharp.cpp)
add_executable(split-tailcall-interface-conservestack split-tailcall-interface-conservestack.cpp)
foreach(_t tailcall-split-fsharp split-tailcall-interface-conservestack)
  target_link_libraries(${_t} PRIVATE mono::warnings)
endforeach()

# One custom command per corpus, declaring every file it produces so the
# assembling step can depend on the individual .il rather than on a stamp.
function(_mono_split_il splitter input outdir)
  set(_outs "")
  foreach(_il IN LISTS MONO_TESTS_IL_SRC)
    if(_il MATCHES "^${outdir}/")
      list(APPEND _outs "${_bin}/${_il}")
    endif()
  endforeach()
  add_custom_command(
    OUTPUT ${_outs}
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_bin}/${outdir}"
    COMMAND sh -c "'$<TARGET_FILE:${splitter}>' < '${_src}/${input}'"
    DEPENDS ${splitter} "${_src}/${input}"
    WORKING_DIRECTORY "${_bin}"
    COMMENT "SPLIT ${input}"
    VERBATIM)
endfunction()

_mono_split_il(tailcall-split-fsharp
               tailcall/fsharp-deeptail.il tailcall/fsharp-deeptail)
_mono_split_il(split-tailcall-interface-conservestack
               tailcall-interface-conservestack.il tailcall/interface-conservestack)

# ---------------------------------------------------------------------------
# Data the tests read at run time.
# ---------------------------------------------------------------------------
set(LIBC "libc.so.6")
set(MONO_NATIVE_LIBRARY_NAME "libmono-native")
set(libsuffix ".so")
configure_file(tests-config.in "${_bin}/tests-config" @ONLY)

file(GLOB _test_configs RELATIVE "${_src}" "*.exe.config" "*-configuration.config")
foreach(_c IN LISTS _test_configs)
  configure_file("${_src}/${_c}" "${_bin}/${_c}" COPYONLY)
endforeach()
configure_file("${_src}/internalsvisibleto-2048.snk" "${_bin}/internalsvisibleto-2048.snk" COPYONLY)
configure_file("${_src}/testing_gac/testkey.snk" "${_bin}/testing_gac/testkey.snk" COPYONLY)

# ---------------------------------------------------------------------------
# Tests automake gave a recipe of their own. Everything named here is skipped
# by the bulk loop below.
# ---------------------------------------------------------------------------
include(special-tests.cmake)

# ---------------------------------------------------------------------------
# The bulk of the corpus: one assembly per source, default references.
# ---------------------------------------------------------------------------
set(_generated_cs array-coop-bigvt.cs array-coop-smallvt.cs array-coop-int.cs
                  runtime-invoke.gen.cs imt_big_iface_test.cs)

# A few sources appear in more than one list (gc-graystack-stress is both a
# stress test and an SGen test, for instance); build each assembly once.
#
# Disabled tests are not built at all, which is what automake did too: it
# filtered them out of the lists before those lists became build targets. Some
# of them no longer compile -- merp-json-valid wants a System.Web.Extensions
# that this profile does not ship -- so building them would break the corpus
# for no gain.
set(_seen ${MONO_TESTS_SPECIAL} ${MONO_TESTS_DISABLED})

# IL first: fourteen entries appear in both lists, naming the same .exe, and
# only the .il exists -- automake resolved them through its %.exe: %.il rule.
foreach(_s IN LISTS MONO_TESTS_IL_SRC MONO_TESTS_TAILCALL_IL_SRC
                    MONO_TESTS_TIER_PINNED_IL_SRC)
  string(REGEX REPLACE "\\.il$" ".exe" _exe "${_s}")
  if(_exe IN_LIST _seen)
    continue()
  endif()
  list(APPEND _seen "${_exe}")
  # The split corpora do not exist until their splitter has run.
  if(_s MATCHES "^tailcall/(fsharp-deeptail|interface-conservestack)/")
    mono_test_assembly("${_exe}" IL SOURCES "${_bin}/${_s}")
  else()
    mono_test_assembly("${_exe}" IL SOURCES "${_s}")
  endif()
endforeach()

foreach(_s IN LISTS MONO_TESTS_CS_SRC MONO_TESTS_BENCH_SRC MONO_TESTS_GSHARED_SRC
                    MONO_TESTS_STRESS_SRC MONO_TESTS_STRESS_PROCESS_SRC
                    MONO_TESTS_SGEN_REGULAR_SRC MONO_TESTS_SGEN_TOGGLEREF_SRC
                    MONO_TESTS_SGEN_BRIDGE_SRC MONO_TESTS_SGEN_BRIDGE2_SRC
                    MONO_TESTS_SGEN_BRIDGE3_SRC MONO_TESTS_SGEN_OOM_SRC
                    MONO_TESTS_UNHANDLED_EXCEPTION_1_SRC
                    MONO_TESTS_UNHANDLED_EXCEPTION_255_SRC
                    MONO_TESTS_FORCED_UNWIND_SRC
                    MONO_TESTS_TIER_PINNED_SRC
                    MONO_TESTS_TAILCALL_CS_SRC)
  string(REGEX REPLACE "\\.cs$" ".exe" _exe "${_s}")
  if(_exe IN_LIST _seen)
    continue()
  endif()
  list(APPEND _seen "${_exe}")
  if(_s IN_LIST _generated_cs)
    mono_test_assembly("${_exe}" SOURCES "${_bin}/${_s}")
  else()
    mono_test_assembly("${_exe}" SOURCES "${_s}")
  endif()
endforeach()

# One source, two assembly names: override-copies.cs loads both and expects the
# override to have replaced the target in each.  A target is matched by name in
# every loaded image rather than by type identity, and this is what tells that
# apart from a table keyed on the first class it saw.
mono_test_assembly(override-copy-a.dll LIBRARY SOURCES override-copy.cs)
mono_test_assembly(override-copy-b.dll LIBRARY SOURCES override-copy.cs)

add_custom_target(runtime-corpora ALL DEPENDS ${_all_assemblies})
add_dependencies(runtime-corpora mono-test-toolchain)

# ---------------------------------------------------------------------------
# CTest wiring
# ---------------------------------------------------------------------------
include(runtime-suites.cmake)

add_subdirectory(gc-descriptors)
add_subdirectory(assembly-load-reference)
add_subdirectory(testing_gac)
