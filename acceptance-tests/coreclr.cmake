# The CoreCLR test corpus.  Rules for the lists in coreclr-tests.cmake.
#
# ~4700 single-file programs from the CoreCLR tree, each compiled to its own
# assembly and run under test-runner.exe.  A CoreCLR test signals success by
# returning 100, hence --expected-exit-code 100.
#
# Two deliberate differences from coreclr.mk:
#
#  * Output goes to the build tree.  automake compiled each Foo.exe next to its
#    Foo.cs inside the checkout and then symlinked coreclr-testlibrary.dll into
#    all ~1480 test directories, because assembly resolution looks in the
#    executable's own directory.  Here the tree is mirrored under
#    ${CMAKE_CURRENT_BINARY_DIR}/coreclr and the test library is reached
#    through MONO_PATH instead, which keeps the checkout clean and drops the
#    symlink farm.  test-runner.exe also writes each test's .stdout/.stderr
#    next to the assembly, so those land in the build tree too.
#
#  * The runtime is named explicitly.  test-runner.exe defaults to `runtime =
#    "mono"`, i.e. whatever is on PATH, and coreclr.mk never passed --runtime.
#    That silently tested the system mono rather than the one just built.

if(NOT MONO_SUBMODULE_CORECLR_PRESENT)
  return()
endif()
if(NOT MONO_SUBMODULE_CORECLR_AT_REV)
  message(WARNING
    "coreclr checkout is at ${MONO_SUBMODULE_CORECLR_HEAD}, expected "
    "${MONO_SUBMODULE_CORECLR_REV}; building the CoreCLR suites anyway.")
endif()

include(coreclr-tests.cmake)

set(_cc_src "${MONO_SUBMODULE_CORECLR_PATH}")
set(_cc_bin "${_bin}/coreclr")

# The nowarn set coreclr.mk passed to every test compile.
set(_cc_nowarn
  -nowarn:0162 -nowarn:0168 -nowarn:0219 -nowarn:0414 -nowarn:0618
  -nowarn:0169 -nowarn:1690 -nowarn:0649 -nowarn:0612 -nowarn:3021
  -nowarn:0197)

# coreclr-testlibrary.dll -- referenced by every C# test
set(_testlib_srcs "")
foreach(_s IN LISTS MONO_CORECLR_TESTLIBRARY_CS_SRC)
  list(APPEND _testlib_srcs "${_cc_src}/${_s}")
endforeach()

add_custom_command(
  OUTPUT "${_bin}/coreclr-testlibrary.dll"
  COMMAND ${_mcs} -unsafe -debug:portable -target:library -d:WINCORESYS -d:MONO
          "-out:${_bin}/coreclr-testlibrary.dll" ${_testlib_srcs}
  DEPENDS ${_testlib_srcs} acceptance-toolchain
  COMMENT "CSC coreclr-testlibrary.dll"
  VERBATIM)

# _coreclr_corpus(<out-list-var> <target> SOURCES ... [IL])
#
# One rule per test.  Expensive to configure -- this is where the bulk of the
# ~4700 build edges come from -- which is the other reason the directory is
# opt-in.
function(_coreclr_corpus out_var stamp_target)
  cmake_parse_arguments(ARG "IL" "" "SOURCES" ${ARGN})

  set(_outs "")
  foreach(_rel IN LISTS ARG_SOURCES)
    get_filename_component(_dir "${_rel}" DIRECTORY)
    # Strip only the final extension, as `$(SRC:.cs=.exe)` did.  Not NAME_WE,
    # which cuts at the *first* dot: that turns csgen.1.cs and csgen.2.cs into
    # the same csgen.exe, and renames the five tests whose basename has more
    # than one dot (test.hfa12.cs, listinsertrange.cs.cs, ...).
    if(ARG_IL)
      string(REGEX REPLACE "\\.il$" "" _stem "${_rel}")
      get_filename_component(_stem "${_stem}" NAME)
      set(_out "${_cc_bin}/${_dir}/${_stem}_il.exe")
      add_custom_command(
        OUTPUT "${_out}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_cc_bin}/${_dir}"
        COMMAND ${_ilasm} "-out:${_out}" "${_cc_src}/${_rel}"
        DEPENDS "${_cc_src}/${_rel}" acceptance-toolchain
        COMMENT "ILASM ${_dir}/${_stem}_il.exe"
        VERBATIM)
    else()
      string(REGEX REPLACE "\\.cs$" "" _stem "${_rel}")
      get_filename_component(_stem "${_stem}" NAME)
      set(_out "${_cc_bin}/${_dir}/${_stem}.exe")
      add_custom_command(
        OUTPUT "${_out}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_cc_bin}/${_dir}"
        COMMAND ${_mcs} -unsafe -debug:portable ${_cc_nowarn}
                -r:coreclr-testlibrary.dll -d:MONO "-out:${_out}"
                "${_cc_src}/${_rel}"
        DEPENDS "${_cc_src}/${_rel}" "${_bin}/coreclr-testlibrary.dll"
                acceptance-toolchain
        WORKING_DIRECTORY "${_bin}"
        COMMENT "CSC ${_dir}/${_stem}.exe"
        VERBATIM)
    endif()
    list(APPEND _outs "${_out}")
  endforeach()

  add_custom_target(${stamp_target} DEPENDS ${_outs})
  set(${out_var} "${_outs}" PARENT_SCOPE)
endfunction()

_coreclr_corpus(_cc_basic_cs   coreclr-corpus-basic-cs
                SOURCES ${MONO_CORECLR_TEST_CS_SRC})
_coreclr_corpus(_cc_basic_il   coreclr-corpus-basic-il   IL
                SOURCES ${MONO_CORECLR_TEST_IL_SRC})
_coreclr_corpus(_cc_coremanglib coreclr-corpus-coremanglib
                SOURCES ${MONO_CORECLR_COREMANGLIB_TEST_CS_SRC})
_coreclr_corpus(_cc_stress     coreclr-corpus-stress
                SOURCES ${MONO_CORECLR_STRESSTEST_CS_SRC})

# `coreclr-compile-tests` in coreclr.mk, which drove make in batches of 100 to
# stay under the shell's argument limit.  Ninja has no such limit and gets the
# whole graph at once.
add_custom_target(coreclr-compile-tests ALL)
add_dependencies(coreclr-compile-tests
  coreclr-corpus-basic-cs coreclr-corpus-basic-il coreclr-corpus-coremanglib
  acceptance-test-runner)

# GCStressTests.exe
set(_gcstress_runner_srcs "")
foreach(_s IN LISTS MONO_CORECLR_STRESSTEST_RUNNER_CS_SRC)
  list(APPEND _gcstress_runner_srcs "${_cc_src}/${_s}")
endforeach()
foreach(_s IN LISTS MONO_CORECLR_STRESSTEST_RUNNER_CS_LOCAL_SRC)
  list(APPEND _gcstress_runner_srcs "${_src}/${_s}")
endforeach()

add_custom_command(
  OUTPUT "${_bin}/GCStressTests.exe"
  COMMAND ${_mcs} "-out:${_bin}/GCStressTests.exe" -debug:portable
          -d:PROJECTK_BUILD ${_gcstress_runner_srcs}
  DEPENDS ${_gcstress_runner_srcs} acceptance-toolchain
  COMMENT "CSC GCStressTests.exe"
  VERBATIM)
add_custom_target(coreclr-gcstress-runner ALL
  DEPENDS "${_bin}/GCStressTests.exe" coreclr-corpus-stress)

# CTest wiring
include(ProcessorCount)
ProcessorCount(_cc_nproc)
if(_cc_nproc EQUAL 0)
  set(_cc_nproc 1)
endif()

# Registers the CTest test name, which runs the tests in TESTS through
# test-runner.exe.  The runner reads them from list_file, one per line.
function(_coreclr_suite name list_file)
  cmake_parse_arguments(ARG "" "" "TESTS" ${ARGN})
  string(REPLACE ";" "\n" _body "${ARG_TESTS}")
  # coreclr.mk wrote this list to a file because the paths blow past the
  # shell's argument limit. Here it is generated once at configure time
  # instead of being appended to in 100-name chunks.
  file(WRITE "${list_file}" "${_body}\n")

  add_test(NAME ${name}
           COMMAND "${CMAKE_COMMAND}" -E env "MONO_PATH=${_class}:${_bin}"
                   "${_wrapper}" --debug "${_bin}/test-runner.exe"
                   -j a
                   --runtime "${_wrapper}"
                   --mono-path "${_class}:${_bin}"
                   --testsuite-name "${name}"
                   --expected-exit-code 100
                   --input-file "${list_file}"
           WORKING_DIRECTORY "${_bin}")
  # test-runner.exe already runs -j a, so claim the machine and stop `ctest -j`
  # from stacking two of these.
  set_tests_properties(${name} PROPERTIES
    LABELS acceptance
    PROCESSORS ${_cc_nproc}
    TIMEOUT 14400)
endfunction()

_coreclr_suite(coreclr "${_bin}/coreclr-testlist.txt"
               TESTS ${_cc_basic_cs} ${_cc_basic_il})
_coreclr_suite(coreclr-coremanglib "${_bin}/coreclr-coremanglib-testlist.txt"
               TESTS ${_cc_coremanglib})

# The GC stress mix.  Driven by its own runner rather than test-runner.exe,
# and it too reports success as 100 -- `if [ $? -ne 100 ]; then exit 1; fi` in
# coreclr.mk -- so it goes through expect-exit.cmake to turn that into the 0
# CTest wants.  Long enough that it is labelled `stress` as well.
add_test(NAME coreclr-gcstress
         COMMAND "${CMAKE_COMMAND}"
                 -D "EXPECT=100"
                 -D "COMMAND=${_wrapper};--debug;${_bin}/GCStressTests.exe;${_cc_src}/tests/src/GC/Stress/testmix_gc_pr.config"
                 -P "${_src}/expect-exit.cmake"
         WORKING_DIRECTORY "${_bin}")
set_tests_properties(coreclr-gcstress PROPERTIES
  LABELS "acceptance;stress"
  PROCESSORS ${_cc_nproc}
  TIMEOUT 21600
  ENVIRONMENT "MONO_PATH=${_class}:${_bin};BVT_ROOT=${_cc_src}/tests/src/GC/Stress/Tests")

# coreclr-list-missing-tests: upstream .cs/.il files this port has never heard
# of.  Useful after bumping the coreclr revision.
set(_known "")
foreach(_l IN ITEMS MONO_CORECLR_TEST_CS_SRC MONO_CORECLR_COREMANGLIB_TEST_CS_SRC
                    MONO_CORECLR_TESTLIBRARY_CS_SRC MONO_CORECLR_STRESSTEST_CS_SRC
                    MONO_CORECLR_DISABLED_TEST_CS_SRC MONO_CORECLR_TEST_IL_SRC
                    MONO_CORECLR_DISABLED_TEST_IL_SRC)
  list(APPEND _known ${${_l}})
endforeach()
string(REPLACE ";" "\n" _known_body "${_known}")
file(WRITE "${_bin}/coreclr-known-tests.txt" "${_known_body}\n")

configure_file("${_src}/coreclr-list-missing.cmake.in"
               "${_bin}/coreclr-list-missing.cmake" @ONLY)
add_custom_target(coreclr-list-missing-tests
  COMMAND "${CMAKE_COMMAND}" -P "${_bin}/coreclr-list-missing.cmake"
  COMMENT "CoreCLR tests missing from coreclr-tests.cmake"
  VERBATIM)
