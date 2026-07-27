# The DebianShootoutMono microbenchmarks, from microbench.mk.
#
# Nine BenchmarkDotNet harnesses driven by a prebuilt DebianShootoutMono.exe
# that ships in the checkout's release/ directory, so there is nothing to
# compile here -- only running and, optionally, profiling.
#
# microbench.mk reached for $(abs_top_srcdir)/runtime/mono-wrapper, which only
# resolves for an in-tree build.  This uses the build tree.
#
# AOT is out of this port's scope, so MONO_BENCH_AOT_RUN/MONO_BENCH_AOT_BUILD
# are empty and `prepare-dlls` has nothing to do -- which is exactly what the
# automake build did too outside the FULL_AOT_TESTS profiles.

if(NOT MONO_SUBMODULE_DEBIANSHOOTOUTMONO_PRESENT)
  return()
endif()

set(_mb_dir "${MONO_SUBMODULE_DEBIANSHOOTOUTMONO_PATH}")
set(_mb_exe "${_mb_dir}/release/DebianShootoutMono.exe")
set(_mb_fix "${_mb_dir}/fixtures")

# name|fixture-file; '|' because ';' is CMake's list separator.  An empty
# fixture means the benchmark takes no input.
set(_microbenchmarks
  "Mandelbrot|"
  "RegexRedux|${_mb_fix}/regexredux-input.txt"
  "KNucleotide|${_mb_fix}/knucleotide-input.txt"
  "BinaryTrees|"
  "NBodyTest|"
  "SpectralNorm|"
  "Fannkuchredux|"
  "Fasta|"
  "RevComp|${_mb_fix}/revcomp-input.txt")

# perf(1) wrapper, for the profiled variants.  microbench-perf.sh.in was an
# AC_CONFIG_FILES output substituting @mono_build_root@.
set(mono_build_root "${CMAKE_BINARY_DIR}")
mono_configure_script("${_src}/microbench-perf.sh.in"
                      "${_bin}/microbench-perf.sh")

find_program(MONO_PERF_BINARY perf DOC "perf(1), for the profiled microbenchmarks")

set(_mb_all "")
foreach(_entry IN LISTS _microbenchmarks)
  string(REGEX REPLACE "\\|.*$" "" _name  "${_entry}")
  string(REGEX REPLACE "^[^|]*\\|" "" _input "${_entry}")

  set(_env
    "MONO_PATH=${_mb_dir}/release:${_class}"
    "MONO_BENCH_AOT_RUN="
    "MONO_BENCH_AOT_BUILD="
    "MONO_BENCH_EXECUTABLE=${_wrapper}"
    "MONO_BENCH_PATH=${_class}"
    "MONO_BENCH_INPUT=${_input}")

  add_custom_target(run-microbench-${_name}
    COMMAND "${CMAKE_COMMAND}" -E env ${_env} "${_wrapper}" "${_mb_exe}" ${_name}
    WORKING_DIRECTORY "${_bin}"
    COMMENT "microbench ${_name}"
    USES_TERMINAL
    VERBATIM)
  list(APPEND _mb_all run-microbench-${_name})

  # BenchmarkDotNet runs each of these for minutes, so they are labelled
  # `slow` on top of `acceptance` and stay out of even `ctest -L acceptance`.
  add_test(NAME microbench-${_name}
           COMMAND "${CMAKE_COMMAND}" -E env ${_env}
                   "${_wrapper}" "${_mb_exe}" ${_name}
           WORKING_DIRECTORY "${_bin}")
  set_tests_properties(microbench-${_name} PROPERTIES
    LABELS "acceptance;slow" TIMEOUT 3600)

  if(MONO_PERF_BINARY)
    # The HOST_LINUX half of microbench.mk: record the benchmark under perf,
    # then turn the profile into a flame graph and a report.  microbench-perf.sh
    # drops perf.data in the acceptance-tests build directory, which is where
    # the makefile's `mv perf.data microbench-results/...` picked it up.
    set(_res "${_bin}/microbench-results")
    add_custom_command(
      OUTPUT "${_res}/${_name}.perf.data"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${_res}"
      COMMAND "${CMAKE_COMMAND}" -E env
              "MONO_PERF_BINARY=${MONO_PERF_BINARY}"
              "MONO_BENCH_EXECUTABLE=${_bin}/microbench-perf.sh"
              "MONO_PATH=${_mb_dir}/release:${_class}"
              "MONO_BENCH_AOT_RUN=" "MONO_BENCH_AOT_BUILD="
              "MONO_BENCH_PATH=${_class}"
              "MONO_BENCH_INPUT=${_input}"
              "${_wrapper}" "${_mb_exe}" ${_name} ${_input}
      COMMAND "${CMAKE_COMMAND}" -E rename "${_bin}/perf.data"
              "${_res}/${_name}.perf.data"
      WORKING_DIRECTORY "${_bin}"
      COMMENT "microbench ${_name} (perf record)"
      USES_TERMINAL
      VERBATIM)

    # MONO_PERF_FLAGS from microbench.mk.
    add_custom_command(
      OUTPUT "${_res}/${_name}.perf.report"
      COMMAND sh -c "'${MONO_PERF_BINARY}' report -i '${_res}/${_name}.perf.data' --show-cpu-utilization -n --hierarchy -T $MONO_PERF_ADDITIONAL_FLAGS > '${_res}/${_name}.perf.report'"
      DEPENDS "${_res}/${_name}.perf.data"
      COMMENT "microbench ${_name} (perf report)"
      VERBATIM)

    add_custom_command(
      OUTPUT "${_res}/${_name}.perf-flame.svg"
      COMMAND sh -c "'${MONO_PERF_BINARY}' script -i '${_res}/${_name}.perf.data' | '${_mb_dir}/FlameGraph/stackcollapse-perf.pl' | '${_mb_dir}/FlameGraph/flamegraph.pl' > '${_res}/${_name}.perf-flame.svg'"
      DEPENDS "${_res}/${_name}.perf.data"
      COMMENT "microbench ${_name} (flame graph)"
      VERBATIM)

    add_custom_target(run-microbench-profiled-${_name}
      DEPENDS "${_res}/${_name}.perf.data")
    add_custom_target(microbench-collect-${_name}
      DEPENDS "${_res}/${_name}.perf.data"
              "${_res}/${_name}.perf.report"
              "${_res}/${_name}.perf-flame.svg")
    list(APPEND _mb_profiled run-microbench-profiled-${_name})
    list(APPEND _mb_collect  microbench-collect-${_name})
  endif()
endforeach()

add_custom_target(test-run-microbench)
add_dependencies(test-run-microbench ${_mb_all})

if(MONO_PERF_BINARY)
  add_custom_target(test-run-microbench-profiled)
  add_dependencies(test-run-microbench-profiled ${_mb_profiled})
  add_custom_target(test-run-microbench-publish-collect)
  add_dependencies(test-run-microbench-publish-collect ${_mb_collect})

  # `test-run-microbench-perf-check` -- whether perf can record at all, which
  # needs perf_event_paranoid low enough or CAP_PERFMON.
  add_custom_target(test-run-microbench-perf-check
    COMMAND "${MONO_PERF_BINARY}" record -a -o "${_bin}/perf-check.data" -- echo testing
    COMMAND "${CMAKE_COMMAND}" -E rm -f "${_bin}/perf-check.data"
    COMMENT "checking perf(1) can record"
    USES_TERMINAL
    VERBATIM)
endif()

# Not ported: `perf-report`/`perf-report-total`, which zipped the .perf.data
# files up for a CI artifact.  `zip microbench-results/*.perf.data` is a
# packaging step, not a build one, and nothing in this tree consumes it.
