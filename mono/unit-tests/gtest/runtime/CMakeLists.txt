# The runtime and the utility libraries under it.

if(NOT MONO_ENABLE_SGEN)
  # The tests link the SGen runtime.
  return()
endif()

# Builds one of this suite's binaries out of SOURCES.  RUNTIME links the whole
# runtime archive, which is what a test that starts a runtime or reaches a
# private symbol needs.
function(mono_add_unit_test name)
  cmake_parse_arguments(ARG "RUNTIME" "" "SOURCES" ${ARGN})

  # Naming the object libraries links only the ones listed here, so mini, the
  # interpreter, the debugger agent and mono-llvm are left out.
  if(ARG_RUNTIME)
    set(_libs monosgen-static)
  else()
    set(_libs monoruntime_sgen monoruntime_config monoruntime_support
              monoculture monosgen monoutils eglib)
  endif()

  mono_gtest_binary(${name}
    SOURCES ${ARG_SOURCES}
    LIBRARIES ${_libs}
    INCLUDES
      "${CMAKE_SOURCE_DIR}/mono"
      "${CMAKE_SOURCE_DIR}/samples"
    DEFINITIONS
      MONO_BINDIR="${CMAKE_INSTALL_FULL_BINDIR}/"
      MONO_ASSEMBLIES="${MONO_ASSEMBLIES_DIR}"
      MONO_CFG_DIR="${MONO_CFG_DIR}"
      MONO_UNIT_TESTS_ASSEMBLIES="${MONO_MCS_LIBDIR}/${MONO_DEFAULT_PROFILE}")
endfunction()

# Registers the cases of target that match filter as ctest tests under prefix,
# with ENV added to each case's environment.
#
# A binary carrying several files' worth of cases gets one call per file.
# That way ctest names stay tied to the source the case lives in, not to
# whichever binary it was linked into.
function(mono_unit_test_cases target prefix filter)
  cmake_parse_arguments(ARG "" "" "ENV" ${ARGN})
  set(_env "MONO_PATH=${MONO_MCS_LIBDIR}/build")
  list(APPEND _env ${ARG_ENV})
  mono_gtest_tests(${target}
    PREFIX ${prefix}
    FILTER ${filter}
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    SKIP_REGEX "\\[  SKIPPED \\]"
    PROPERTIES ENVIRONMENT "${_env}")
endfunction()

# The tests that need neither a runtime nor the thread-info subsystem, plus the
# one -- ConcHashTable -- that stands it up first.
mono_add_unit_test(test-mono-utils SOURCES
  test-sgen-qsort.cpp test-memfuncs.cpp test-mono-handle.cpp
  test-conc-hashtable.cpp)
mono_unit_test_cases(test-mono-utils test-sgen-qsort     "SgenQsort.*")
mono_unit_test_cases(test-mono-utils test-memfuncs       "MemFuncs.*")
mono_unit_test_cases(test-mono-utils test-mono-handle    "MonoHandle.*")
mono_unit_test_cases(test-mono-utils test-conc-hashtable "ConcHashTable.*")

# Alone because it wants mono_thread_info_init (0) where ConcHashTable wants
# sizeof (MonoThreadInfo), and whichever runs first decides for the process.
mono_add_unit_test(test-mono-linked-list-set SOURCES test-mono-linked-list-set.cpp)
mono_unit_test_cases(test-mono-linked-list-set test-mono-linked-list-set "*")

# The tests that run against a live runtime, which harness.cpp brings up once
# for the binary. test-path is here without needing one. It reaches
# mono-path.c's statics by including the file. Only the archive semantics of
# the RUNTIME link let the linker leave the library's own copy of those
# symbols out.
mono_add_unit_test(test-mono-runtime RUNTIME SOURCES
  harness.cpp test-mono-callspec.cpp test-mono-string.cpp test-path.cpp
  test-function-pointer.cpp test-detour.cpp test-icall.cpp
  test-unmanaged-callers-only.cpp test-arch-amd64.cpp
  test-unity-pinned-alloc.cpp)
mono_unit_test_cases(test-mono-runtime test-mono-callspec "Callspec.*")
mono_unit_test_cases(test-mono-runtime test-mono-string   "MonoStringTest.*")
mono_unit_test_cases(test-mono-runtime test-path          "MonoPath.*")
mono_unit_test_cases(test-mono-runtime test-function-pointer "FunctionPointer.*")
mono_unit_test_cases(test-mono-runtime test-detour        "MethodDetour.*")
mono_unit_test_cases(test-mono-runtime test-icall         "NoWrapperIcall.*")
mono_unit_test_cases(test-mono-runtime test-unmanaged-callers-only
                     "UnmanagedCallersOnly.*")
mono_unit_test_cases(test-mono-runtime test-arch-amd64    "ArchAmd64.*")
mono_unit_test_cases(test-mono-runtime test-unity-pinned-alloc
                     "UnityPinnedAlloc.*")

# The cases above that read a tier-0 caller's behaviour, again with the
# interpreter as tier 0. The default arms run the classic compiler there, and
# the two engines answer some of these differently: an interpreted caller
# misses a detour on a callee it copied in, where a classic one folds nothing.
set(_interp_tier0 "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier0-classic=0")
mono_unit_test_cases(test-mono-runtime test-function-pointer-interp
                     "FunctionPointer.*" ENV "${_interp_tier0}")
mono_unit_test_cases(test-mono-runtime test-detour-interp
                     "MethodDetour.*" ENV "${_interp_tier0}")
mono_unit_test_cases(test-mono-runtime test-unmanaged-callers-only-interp
                     "UnmanagedCallersOnly.*" ENV "${_interp_tier0}")

# One case each, and there cannot be a second. The samples they are built out
# of run a domain to completion and call mono_jit_cleanup. Nothing in the
# process gets a runtime after that. They take harness.cpp for its
# class-library check alone -- the runtime they use is the one the sample
# starts itself.
mono_add_unit_test(test-mono-embed   SOURCES harness.cpp test-mono-embed.cpp   RUNTIME)
mono_unit_test_cases(test-mono-embed test-mono-embed "*")
mono_add_unit_test(test-embed-invoke SOURCES harness.cpp test-embed-invoke.cpp RUNTIME)
mono_unit_test_cases(test-embed-invoke test-embed-invoke "*")

# Its own binary: check-remset-consistency reads MONO_GC_DEBUG once at process
# start, and test-mono-runtime's cases share one process.
#
# With MONO_SGEN_STRICT_REMSET_CHECK on, a confirmed miss aborts the process by
# design. ctest counts a signal death as a failure before it reads
# PASS_REGULAR_EXPRESSION at all (ctest 3.28), so check-remset-missing.sh
# judges the outcome instead.
mono_add_unit_test(test-remset-missing SOURCES harness.cpp test-remset-missing.cpp RUNTIME)
if(MONO_SGEN_STRICT_REMSET_CHECK)
  set(_remset_expect on)
else()
  set(_remset_expect off)
endif()
add_test(NAME test-remset-missing
         COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/check-remset-missing.sh"
                 "$<TARGET_FILE:test-remset-missing>" "${_remset_expect}"
         WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(test-remset-missing PROPERTIES
  ENVIRONMENT "MONO_GC_DEBUG=check-remset-consistency;MONO_PATH=${MONO_MCS_LIBDIR}/build"
  SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\]")

# The managed side of the tests that need one.
if(MONO_ENABLE_MCS_BUILD)
  set(_class_dir "${MONO_MCS_LIBDIR}/${MONO_DEFAULT_PROFILE}")
  set(_build_dir "${MONO_MCS_LIBDIR}/build")
  mono_tools_runtime(_host MONO_PATH "${_build_dir}")
  set(_mcs ${_host}
           "${MONO_CSC}" -debug:portable -noconfig -nologo -unsafe
           -nowarn:0162 -nowarn:0168 -nowarn:0219 -nowarn:0414 -nowarn:0618
           -nowarn:0169 -nowarn:1690 -nowarn:0649 -nowarn:0612 -nowarn:3021
           -nowarn:0197
           "-r:${_class_dir}/System.dll"
           "-r:${_class_dir}/System.Xml.dll"
           "-r:${_class_dir}/System.Core.dll")

  function(_mono_test_assembly out source)
    add_custom_command(
      OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${out}"
      COMMAND ${_mcs} "-out:${CMAKE_CURRENT_BINARY_DIR}/${out}" "${source}"
      DEPENDS "${source}" mcs
      COMMENT "CSC ${out}"
      VERBATIM)
  endfunction()

  _mono_test_assembly(callspec.exe "${CMAKE_CURRENT_SOURCE_DIR}/callspec.cs")
  _mono_test_assembly(detour.exe "${CMAKE_CURRENT_SOURCE_DIR}/detour.cs")
  _mono_test_assembly(icall.exe "${CMAKE_CURRENT_SOURCE_DIR}/icall.cs")
  _mono_test_assembly(unmanaged-callers-only.exe
                      "${CMAKE_CURRENT_SOURCE_DIR}/unmanaged-callers-only.cs")
  _mono_test_assembly(remset-missing.exe "${CMAKE_CURRENT_SOURCE_DIR}/remset-missing.cs")
  _mono_test_assembly(unity-pinned-alloc.exe "${CMAKE_CURRENT_SOURCE_DIR}/unity-pinned-alloc.cs")
  _mono_test_assembly(test-embed.exe "${CMAKE_SOURCE_DIR}/samples/embed/test.cs")
  _mono_test_assembly(test-embed-invoke-cs.exe "${CMAKE_SOURCE_DIR}/samples/embed/invoke.cs")

  add_custom_target(unit-test-assemblies ALL DEPENDS
    "${CMAKE_CURRENT_BINARY_DIR}/callspec.exe"
    "${CMAKE_CURRENT_BINARY_DIR}/detour.exe"
    "${CMAKE_CURRENT_BINARY_DIR}/icall.exe"
    "${CMAKE_CURRENT_BINARY_DIR}/unmanaged-callers-only.exe"
    "${CMAKE_CURRENT_BINARY_DIR}/remset-missing.exe"
    "${CMAKE_CURRENT_BINARY_DIR}/unity-pinned-alloc.exe"
    "${CMAKE_CURRENT_BINARY_DIR}/test-embed.exe"
    "${CMAKE_CURRENT_BINARY_DIR}/test-embed-invoke-cs.exe")
  mono_tools_runtime_depends(unit-test-assemblies)
endif()
