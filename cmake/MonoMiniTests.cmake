# The mini regression suites: `ctest -L regression`, the shared corpora on the
# JIT. See MonoCorpus.cmake for the shared corpus plumbing.

if(NOT MONO_CORPUS_ENABLED)
  return()
endif()

set(_class_dir "${MONO_CORPUS_CLASS_DIR}")
set(_wrapper   "${MONO_CORPUS_WRAPPER}")

set(MONO_CORPUS_OUTPUTS "")

set(_driver "${CMAKE_CURRENT_BINARY_DIR}/TestDriver.dll")
mono_corpus_cs(TestDriver.dll LIBRARY SOURCES TestDriver.cs TestHelpers.cs)
mono_corpus_il(generics-variant-types.dll generics-variant-types.il LIBRARY)
mono_corpus_il(MemoryIntrinsics.dll       MemoryIntrinsics.il       LIBRARY)

# Plain "compile against TestDriver" corpora.
foreach(_t basic basic-float basic-long basic-calls objects arrays basic-math
           exceptions devirtualization gshared aot-tests ratests)
  mono_corpus_cs(${_t}.exe SOURCES ${_t}.cs REFS "${_driver}")
endforeach()

# Corpora with extra references.
mono_corpus_cs(builtin-types.exe SOURCES builtin-types.cs REFS "${_driver}")
mono_corpus_cs(basic-simd.exe    SOURCES basic-simd.cs
               REFS "${_driver}" "${_class_dir}/Mono.Simd.dll")
mono_corpus_cs(basic-vectors.exe SOURCES basic-vectors.cs
               REFS "${_driver}" "${_class_dir}/System.Numerics.dll"
                    "${_class_dir}/System.Numerics.Vectors.dll")
mono_corpus_cs(generics.exe      SOURCES generics.cs
               REFS "${_driver}"
                    "${CMAKE_CURRENT_BINARY_DIR}/generics-variant-types.dll"
                    "${_class_dir}/System.Core.dll")
mono_corpus_cs(unaligned.exe     SOURCES unaligned.cs
               REFS "${_driver}" "${CMAKE_CURRENT_BINARY_DIR}/MemoryIntrinsics.dll")
mono_corpus_il(iltests.exe iltests.il)
mono_corpus_cs(tier-seam.exe SOURCES tier-seam.cs
               REFS "${_driver}" "${_class_dir}/System.Numerics.dll"
                    "${_class_dir}/System.Numerics.Vectors.dll")
mono_corpus_cs(xdomain.exe   SOURCES xdomain.cs)

add_custom_target(mini-corpora ALL DEPENDS ${MONO_CORPUS_OUTPUTS})

# The corpora under mono/unit-tests reference this and are built in their own
# directories, so they need it finished first.  A target-level dependency is the
# only kind that crosses directories reliably.
add_custom_target(mini-test-driver DEPENDS "${_driver}")

# ---------------------------------------------------------------------------
# CTest wiring
# ---------------------------------------------------------------------------
set(_regtests
  aot-tests.exe basic.exe basic-float.exe basic-long.exe basic-calls.exe
  builtin-types.exe gshared.exe objects.exe arrays.exe basic-math.exe
  exceptions.exe iltests.exe devirtualization.exe generics.exe basic-simd.exe
  unaligned.exe basic-vectors.exe ratests.exe)

# One test per corpus, so a failure names the corpus and --rerun-failed re-runs
# only what broke.
#
# Tier 0 is off here. --regression calls each test method once, so nothing ever
# spends its call counter and the whole corpus would run interpreted -- which is
# what the suite below is for.
foreach(_t IN LISTS _regtests)
  string(REGEX REPLACE "\\.exe$" "" _stem "${_t}")
  add_test(NAME "mini-regression/${_stem}"
           COMMAND "${CMAKE_COMMAND}" -E env "MONO_PATH=${_class_dir}"
                   "MONO_LLVM_JIT_TIER0=0"
                   "${_wrapper}" --regression ${_t}
           WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties("mini-regression/${_stem}" PROPERTIES LABELS regression)
endforeach()

# The same corpora at the default tier: every method the interpreter accepts
# runs interpreted, and everything else compiles and calls into it. That
# crossing is where a method's entries have to agree with the convention its
# callers were compiled against, and nothing else covers it at this scale - the
# suite above compiles everything, and the interpreter's own harness interprets
# everything.
if(MONO_ENABLE_INTERPRETER)
  foreach(_t IN LISTS _regtests)
    string(REGEX REPLACE "\\.exe$" "" _stem "${_t}")
    add_test(NAME "mini-regression-interp/${_stem}"
             COMMAND "${CMAKE_COMMAND}" -E env "MONO_PATH=${_class_dir}"
                     "${_wrapper}" --regression ${_t}
             WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties("mini-regression-interp/${_stem}"
                         PROPERTIES LABELS interp)
  endforeach()
endif()

# The interpreted-caller/compiled-callee crossing, which neither suite above
# reaches: `mini-regression` compiles everything, and `mini-regression-interp`
# runs at the default tier but carries the `interp` label, which `check` drops.
# This one keeps `regression` so the crossing is covered by the fast set -- it
# is where a callee's prototype has to agree with what its caller was compiled
# against, and getting that wrong is a wrong register rather than a diagnostic.
#
# The corpus loops until its callees are compiled underneath it, so the run has
# to prove it got that far; MonoRunTracedTest fails it if any callee was never
# compiled, which is what a loop that finished too early looks like.
if(MONO_ENABLE_INTERPRETER)
  add_test(NAME "mini-regression/tier-seam"
           COMMAND "${CMAKE_COMMAND}" -E env "MONO_PATH=${_class_dir}"
                   "${CMAKE_COMMAND}"
                   "-DMONO_TRACE_REQUIRE=Tests:wide_static_noargs;Tests:wide_static_onearg;Tests:wide_instance_noargs;Tests:narrow_static_noargs;Tests:simd_roundtrip;Tests:quad_roundtrip"
                   -P "${CMAKE_SOURCE_DIR}/cmake/MonoRunTracedTest.cmake"
                   -- "${_wrapper}" --regression tier-seam.exe
           WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties("mini-regression/tier-seam" PROPERTIES LABELS regression)
endif()

# The same corpus with the interpreter out of the way, so a failure above says
# whether the crossing broke it or the code generated for it is simply wrong.
add_test(NAME "mini-regression/tier-seam-compiled"
         COMMAND "${CMAKE_COMMAND}" -E env "MONO_PATH=${_class_dir}"
                 "MONO_LLVM_JIT_TIER0=0"
                 "${_wrapper}" --regression tier-seam.exe
         WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties("mini-regression/tier-seam-compiled" PROPERTIES LABELS regression)

# Cross-domain calls, for the frame the interpreter builds with no InterpMethod
# behind it. The crossing happens on the first call rather than after a compile,
# so this pair needs nothing to show it got far enough.
#
# The corpus directory joins MONO_PATH because the child domain resolves the
# program by assembly name rather than by the path it was started from.
if(MONO_ENABLE_INTERPRETER)
  add_test(NAME "mini-regression/xdomain"
           COMMAND "${CMAKE_COMMAND}" -E env
                   "MONO_PATH=${_class_dir}:${CMAKE_CURRENT_BINARY_DIR}"
                   "${_wrapper}" xdomain.exe
           WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties("mini-regression/xdomain" PROPERTIES LABELS regression)
endif()

add_test(NAME "mini-regression/xdomain-compiled"
         COMMAND "${CMAKE_COMMAND}" -E env
                 "MONO_PATH=${_class_dir}:${CMAKE_CURRENT_BINARY_DIR}"
                 "MONO_LLVM_JIT_TIER0=0"
                 "${_wrapper}" xdomain.exe
         WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties("mini-regression/xdomain-compiled" PROPERTIES LABELS regression)
