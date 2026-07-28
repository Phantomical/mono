# The mini regression suites.
#
# Two entry points, matching what the automake build called `rcheck` and
# `tieredcheck`:
#
#   ctest -L regression   classic JIT, --nollvm, the shared corpora
#   ctest -L tiered       LLVM tier-1 at a range of promotion thresholds
#
# The corpora are C#/IL compiled by the class-library toolchain, so they can
# only be built after mcs has run.  They are wired as a CTest fixture rather
# than as part of `all`: a plain build of the runtime should not have to wait
# for the compiler.

if(NOT MONO_ENABLE_MCS_BUILD OR NOT MONO_ENABLE_EXECUTABLES)
  return()
endif()

set(_class_dir  "${MONO_MCS_TOPDIR}/class/lib/${MONO_DEFAULT_PROFILE}")
set(_build_dir  "${MONO_MCS_TOPDIR}/class/lib/build")
set(_wrapper    "${CMAKE_BINARY_DIR}/runtime/mono-wrapper")
set(_csflags    -unsafe -nowarn:0219,0169,0414,0649,0618)

# csc and ilasm run on whichever mono MONO_USE_SYSTEM_RUNTIME_FOR_TOOLS selected;
# the corpora themselves are always compiled -nostdlib against this tree's
# mscorlib, so the host makes no difference to what comes out.
mono_tools_runtime(_host MONO_PATH "${_build_dir}")
set(_mcs   ${_host}
           "${MONO_CSC}" -langversion:8.0 -nostdlib -unsafe -nowarn:0162 -nologo -noconfig
           "-r:${_class_dir}/mscorlib.dll"
           "-r:${_class_dir}/System.dll"
           "-r:${_class_dir}/System.Core.dll")
set(_ilasm ${_host} "${_build_dir}/ilasm.exe")

set(_corpora_outputs "")

function(_mono_add_cs_target out)
  cmake_parse_arguments(ARG "LIBRARY" "" "SOURCES;REFS" ${ARGN})
  set(_extra "")
  if(ARG_LIBRARY)
    set(_extra -target:library)
  endif()
  set(_srcs "")
  foreach(_s IN LISTS ARG_SOURCES)
    list(APPEND _srcs "${CMAKE_CURRENT_SOURCE_DIR}/${_s}")
  endforeach()
  # A reference that is itself built here (TestDriver.dll and friends) has to be
  # a dependency too, or a parallel build compiles against a file that does not
  # exist yet.  References into the class libraries are already covered by the
  # mcs dependency on the mini-corpora target.
  set(_refs "")
  set(_ref_deps "")
  foreach(_r IN LISTS ARG_REFS)
    list(APPEND _refs "-r:${_r}")
    if(_r MATCHES "^${CMAKE_CURRENT_BINARY_DIR}/")
      list(APPEND _ref_deps "${_r}")
    endif()
  endforeach()
  add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${out}"
    COMMAND ${_mcs} ${_extra} ${_csflags} "-out:${CMAKE_CURRENT_BINARY_DIR}/${out}"
            ${_srcs} ${_refs}
    DEPENDS ${_srcs} ${_ref_deps}
    COMMENT "CSC ${out}"
    VERBATIM)
  set(_corpora_outputs "${_corpora_outputs};${CMAKE_CURRENT_BINARY_DIR}/${out}" PARENT_SCOPE)
endfunction()

function(_mono_add_il_target out source)
  cmake_parse_arguments(ARG "LIBRARY" "" "" ${ARGN})
  set(_extra "")
  if(ARG_LIBRARY)
    set(_extra -dll)
  endif()
  get_filename_component(_outdir "${CMAKE_CURRENT_BINARY_DIR}/${out}" DIRECTORY)
  add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${out}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_outdir}"
    COMMAND ${_ilasm} ${_extra} "-output=${CMAKE_CURRENT_BINARY_DIR}/${out}"
            "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
    COMMENT "ILASM ${out}"
    VERBATIM)
  set(_corpora_outputs "${_corpora_outputs};${CMAKE_CURRENT_BINARY_DIR}/${out}" PARENT_SCOPE)
endfunction()

set(_driver "${CMAKE_CURRENT_BINARY_DIR}/TestDriver.dll")
_mono_add_cs_target(TestDriver.dll LIBRARY SOURCES TestDriver.cs TestHelpers.cs)
_mono_add_il_target(generics-variant-types.dll generics-variant-types.il LIBRARY)
_mono_add_il_target(MemoryIntrinsics.dll       MemoryIntrinsics.il       LIBRARY)
_mono_add_il_target(inliner-fault.dll          inliner-fault.il          LIBRARY)

# Plain "compile against TestDriver" corpora.
foreach(_t basic basic-float basic-long basic-calls objects arrays basic-math
           exceptions devirtualization gshared aot-tests ratests
           tiered-promotion tiered-decline tiered-appdomain il-offset-tests)
  _mono_add_cs_target(${_t}.exe SOURCES ${_t}.cs REFS "${_driver}")
endforeach()

# Corpora with extra references.
_mono_add_cs_target(builtin-types.exe SOURCES builtin-types.cs REFS "${_driver}")
_mono_add_cs_target(basic-simd.exe    SOURCES basic-simd.cs
                    REFS "${_driver}" "${_class_dir}/Mono.Simd.dll")
_mono_add_cs_target(basic-vectors.exe SOURCES basic-vectors.cs
                    REFS "${_driver}" "${_class_dir}/System.Numerics.dll"
                         "${_class_dir}/System.Numerics.Vectors.dll")
_mono_add_cs_target(generics.exe      SOURCES generics.cs
                    REFS "${_driver}"
                         "${CMAKE_CURRENT_BINARY_DIR}/generics-variant-types.dll"
                         "${_class_dir}/System.Core.dll")
_mono_add_cs_target(unaligned.exe     SOURCES unaligned.cs
                    REFS "${_driver}" "${CMAKE_CURRENT_BINARY_DIR}/MemoryIntrinsics.dll")
_mono_add_cs_target(inliner-tests.exe SOURCES inliner-tests.cs
                    REFS "${_driver}" "${CMAKE_CURRENT_BINARY_DIR}/inliner-fault.dll")
_mono_add_il_target(iltests.exe iltests.il)

# Not dependent on `mcs` -- see the note in mono/tests/CMakeLists.txt.
add_custom_target(mini-corpora DEPENDS ${_corpora_outputs})

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

# A tiered run is not a one-core test: besides the mutator it starts a pool of
# background compile workers, sized by tiered_compile_thread_count () in
# mono/mini/llvm/tiered.cpp.  Mirror that rule here so `ctest -j N` schedules
# against the cores these tests really use.  Without it ctest counts each run as
# one processor and packs N of them onto N cores, oversubscribing by the pool
# size -- and since every assertion about promotion is a bounded wait for work
# that happens on those very workers, the suite then fails on machine load
# rather than on anything it is testing.
#
# The count only has to be right on the machine that runs the tests, which is
# the one configuring: it is a scheduling hint, not a correctness contract.
include(ProcessorCount)
ProcessorCount(_ncpu)
if(_ncpu EQUAL 0)
  set(_ncpu 1)
endif()
math(EXPR _tiered_workers "${_ncpu} / 4")
if(_tiered_workers LESS 1)
  set(_tiered_workers 1)
elseif(_tiered_workers GREATER 4)
  set(_tiered_workers 4)
endif()
math(EXPR _tiered_procs "${_tiered_workers} + 1")   # + the mutator

# One test per (corpus, threshold) pair so a failure names the configuration
# that broke rather than just "tiered".
function(_mono_add_tiered_test group corpus threshold)
  cmake_parse_arguments(ARG "" "ENV" "ARGS" ${ARGN})
  set(_env "MONO_PATH=${_class_dir}" "MONO_TIERED=1"
           "MONO_TIERED_CALL_THRESHOLD=${threshold}")
  if(ARG_ENV)
    list(APPEND _env "${ARG_ENV}")
  endif()
  add_test(NAME tiered-${group}-${threshold}
           COMMAND "${CMAKE_COMMAND}" -E env ${_env}
                   "${_wrapper}" --llvm ${ARG_ARGS} ${corpus}
           WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  # Threshold 0 is eager: it promotes inline on the mutator and never starts the
  # pool, so it really is a one-core run.
  set(_procs ${_tiered_procs})
  if(threshold EQUAL 0)
    set(_procs 1)
  endif()
  set_tests_properties(tiered-${group}-${threshold} PROPERTIES
    LABELS tiered FIXTURES_REQUIRED mini_corpora PROCESSORS ${_procs})
endfunction()

if(MONO_ENABLE_LLVM)
  foreach(_n 1000 20 1)
    _mono_add_tiered_test(promotion tiered-promotion.exe ${_n} ARGS --regression)
    _mono_add_tiered_test(inliner-regression inliner-tests.exe ${_n} ARGS --regression)
  endforeach()
  # The same corpus again under the runtime's DEFAULT optimization set, which is
  # the only way these runs get MONO_OPT_GSHARED: --regression drives its own
  # list of opt combinations and all but one of them leave gshared out, and the
  # first combination to run has already JIT'd -- and latched the tier state of
  # -- every method by the time the one that includes it comes around. Methods
  # that are only interesting when compiled shared are therefore untested by the
  # --regression runs above, however many thresholds they sweep.
  foreach(_n 1000 20 1 0)
    _mono_add_tiered_test(promotion-gshared tiered-promotion.exe ${_n})
    _mono_add_tiered_test(inliner inliner-tests.exe ${_n})
  endforeach()
  # The decline corpus needs tier-1 to never actually fire, so point the
  # one-method allowlist at a name nothing matches.
  _mono_add_tiered_test(decline tiered-decline.exe 20 ARGS --regression
                        ENV "MONO_LLVM_METHOD=TieredDecline:NeverMatchesAnything")
  _mono_add_tiered_test(appdomain tiered-appdomain.exe 1)
  foreach(_n 0 1)
    _mono_add_tiered_test(exceptions exceptions.exe ${_n} ARGS --regression)
    _mono_add_tiered_test(iltests    iltests.exe    ${_n} ARGS --regression)
  endforeach()

  # The corpus only ever checks that tier-1 code computes the right answer,
  # which it does whether or not anything was inlined -- so this asserts the
  # decisions themselves, read out of the pass's own trace, against expectations
  # written next to the fixtures. Without it an inliner that quietly stands down
  # is a green run; that has happened before.  The script evals its first
  # argument as a command prefix, environment assignments and all.
  add_test(NAME tiered-inliner-tags
           COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/check-inliner-tags.sh"
                   "MONO_PATH=${_class_dir} ${_wrapper}"
                   "${CMAKE_CURRENT_BINARY_DIR}/inliner-tests.exe"
                   "${CMAKE_CURRENT_SOURCE_DIR}/inliner-tests.cs"
           WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties(tiered-inliner-tags PROPERTIES
    LABELS tiered FIXTURES_REQUIRED mini_corpora)

  # Tier 1 reads its native_offset -> il_offset map back out of the emitted
  # object rather than building it during codegen, so the map can be wrong --
  # or carry a body it inlined -- with every corpus still green, because a bad
  # map only makes stack traces lie. This diffs it against the classic JIT,
  # which computes the same mapping the direct way.
  add_test(NAME tiered-il-offsets
           COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/check-il-offsets.sh"
                   "MONO_PATH=${_class_dir} ${_wrapper}"
                   "${CMAKE_CURRENT_BINARY_DIR}/il-offset-tests.exe"
           WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties(tiered-il-offsets PROPERTIES
    LABELS tiered FIXTURES_REQUIRED mini_corpora)
endif()

# ---------------------------------------------------------------------------
# The interpreter whitebox test: a C driver that reaches into the interpreter's
# internals, so it links libmini rather than running under the mono binary.
# ---------------------------------------------------------------------------
if(MONO_ENABLE_INTERPRETER)
  _mono_add_il_target(interp/whitebox-snippets.exe interp/whitebox-snippets.il)

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
  # corpus, and its snippets assembly is not in ${_corpora_outputs}.
  add_custom_target(mini-whitebox-snippets
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/interp/whitebox-snippets.exe")
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
