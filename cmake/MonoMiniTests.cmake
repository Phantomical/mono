# The mini regression suites.
#
# Two entry points, matching what the automake build called `rcheck` and
# `tieredcheck`:
#
#   ctest -L regression   classic JIT, --nollvm, the shared corpora
#   ctest -L tiered       LLVM tier-1 at a range of promotion thresholds
#
# The corpora that exist to pin down what a single tier-1 pass decided live with
# their check under mono/unit-tests instead; see MonoCorpus.cmake for the pieces
# both halves share.

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
           exceptions devirtualization gshared aot-tests ratests
           tiered-promotion tiered-decline tiered-appdomain)
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

# Not dependent on `mcs` -- see the note in mono/tests/CMakeLists.txt.
add_custom_target(mini-corpora DEPENDS ${MONO_CORPUS_OUTPUTS})

# The corpora under mono/unit-tests reference this and are built in their own
# directories, so they need it finished first.  A target-level dependency is the
# only kind that crosses directories reliably.
add_custom_target(mini-test-driver DEPENDS "${_driver}")

# ---------------------------------------------------------------------------
# CTest wiring
# ---------------------------------------------------------------------------
# CTest cannot build, so building the corpora is itself a test that everything
# else depends on through a fixture.
add_test(NAME mini-corpora
         COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target mini-corpora)
set_tests_properties(mini-corpora PROPERTIES FIXTURES_SETUP mini_corpora LABELS fixture)

set(_regtests
  aot-tests.exe basic.exe basic-float.exe basic-long.exe basic-calls.exe
  builtin-types.exe gshared.exe objects.exe arrays.exe basic-math.exe
  exceptions.exe iltests.exe devirtualization.exe generics.exe basic-simd.exe
  unaligned.exe basic-vectors.exe ratests.exe)

# The sequence-point check under mono/unit-tests runs over the same list.
set(MONO_MINI_REGTESTS ${_regtests} CACHE INTERNAL "the mini regression corpora")

# --nollvm is deliberate.  LLVM and tiering are on by default, so without it
# this suite would stop being the classic-JIT baseline it exists to be and a
# tier-0 regression could hide behind a tier-1 body.  The default configuration
# is what the tiered tests below cover.
add_test(NAME mini-regression
         COMMAND "${CMAKE_COMMAND}" -E env "MONO_PATH=${_class_dir}"
                 "${_wrapper}" --nollvm --regression ${_regtests}
         WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(mini-regression PROPERTIES
  LABELS regression FIXTURES_REQUIRED mini_corpora)

if(MONO_ENABLE_LLVM)
  foreach(_n 1000 20 1)
    mono_add_tiered_test(promotion tiered-promotion.exe ${_n} ARGS --regression)
  endforeach()
  # The same corpus again under the runtime's DEFAULT optimization set, which is
  # the only way these runs get MONO_OPT_GSHARED: --regression drives its own
  # list of opt combinations and all but one of them leave gshared out, and the
  # first combination to run has already JIT'd -- and latched the tier state of
  # -- every method by the time the one that includes it comes around. Methods
  # that are only interesting when compiled shared are therefore untested by the
  # --regression runs above, however many thresholds they sweep.
  foreach(_n 1000 20 1 0)
    mono_add_tiered_test(promotion-gshared tiered-promotion.exe ${_n})
  endforeach()
  # The decline corpus needs tier-1 to never actually fire, so point the
  # one-method allowlist at a name nothing matches.
  mono_add_tiered_test(decline tiered-decline.exe 20 ARGS --regression
                       ENV "MONO_LLVM_METHOD=TieredDecline:NeverMatchesAnything")
  mono_add_tiered_test(appdomain tiered-appdomain.exe 1)
  foreach(_n 0 1)
    mono_add_tiered_test(exceptions exceptions.exe ${_n} ARGS --regression)
    mono_add_tiered_test(iltests    iltests.exe    ${_n} ARGS --regression)
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

  # The fixture builds the corpora, but this pair is declared after the target,
  # so hang them off it explicitly: the driver is a program rather than a
  # corpus, and its snippets assembly is not in the mini corpus list.
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
    LABELS manual FIXTURES_REQUIRED mini_corpora)
endif()
