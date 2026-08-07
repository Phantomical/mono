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
foreach(_t IN LISTS _regtests)
  string(REGEX REPLACE "\\.exe$" "" _stem "${_t}")
  add_test(NAME "mini-regression/${_stem}"
           COMMAND "${CMAKE_COMMAND}" -E env "MONO_PATH=${_class_dir}"
                   "${_wrapper}" --regression ${_t}
           WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties("mini-regression/${_stem}" PROPERTIES LABELS regression)
endforeach()

# The same corpora with both engines in one process: every method the
# interpreter accepts runs interpreted, and everything else compiles and calls
# into it. That crossing is where a method's entries have to agree with the
# convention its callers were compiled against, and nothing else covers it at
# this scale - the suite above compiles everything, and the interpreter's own
# harness interprets everything.
if(MONO_ENABLE_INTERPRETER)
  foreach(_t IN LISTS _regtests)
    string(REGEX REPLACE "\\.exe$" "" _stem "${_t}")
    add_test(NAME "mini-regression-interp/${_stem}"
             COMMAND "${CMAKE_COMMAND}" -E env "MONO_PATH=${_class_dir}"
                     "${_wrapper}" --interp-tier0 --regression ${_t}
             WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties("mini-regression-interp/${_stem}"
                         PROPERTIES LABELS interp)
  endforeach()
endif()

# ---------------------------------------------------------------------------
# The interpreter whitebox test: a C driver that reaches into the interpreter's
# internals, so it links libmini rather than running under the mono binary.
# ---------------------------------------------------------------------------
if(MONO_ENABLE_INTERPRETER)
  set(MONO_CORPUS_OUTPUTS "")
  mono_corpus_il(interp/whitebox-snippets.exe interp/whitebox-snippets.il)

  add_executable(test-mono-interp-whitebox interp/whitebox.c)
  target_include_directories(test-mono-interp-whitebox PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}"
    "${CMAKE_SOURCE_DIR}")
  target_link_libraries(test-mono-interp-whitebox PRIVATE
    mono::common mono::hidden mono::eglib_headers)
  foreach(_o IN LISTS MONO_SGEN_OBJECTS)
    target_link_libraries(test-mono-interp-whitebox PRIVATE ${_o})
  endforeach()
  target_link_libraries(test-mono-interp-whitebox PRIVATE mono::llvm ${_mono_zlib} m)
  set_target_properties(test-mono-interp-whitebox PROPERTIES LINKER_LANGUAGE CXX)

  # This pair is declared after the mini-corpora target, so hang them off it
  # explicitly: the driver is a program rather than a corpus, and its snippets
  # assembly is not in the mini corpus list.
  add_custom_target(mini-whitebox-snippets DEPENDS ${MONO_CORPUS_OUTPUTS})
  add_dependencies(mini-corpora mini-whitebox-snippets test-mono-interp-whitebox)

  add_test(NAME interp-whitebox
           COMMAND "${CMAKE_COMMAND}" -E env "MONO_PATH=${_class_dir}"
                   $<TARGET_FILE:test-mono-interp-whitebox>
                   "${CMAKE_CURRENT_BINARY_DIR}/interp/whitebox-snippets.exe"
           WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  # Disabled, not deleted.  automake had `interp-whitebox` as a target you ran
  # by hand -- it was never part of `make check` -- and the driver currently
  # segfaults on startup with both build systems, so running it by default
  # would just paint the suite red for a pre-existing runtime bug.  Re-enable
  # by clearing DISABLED once that is fixed.
  set_tests_properties(interp-whitebox PROPERTIES
    DISABLED TRUE
    LABELS manual)
endif()
