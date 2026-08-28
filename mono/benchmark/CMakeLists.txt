# JIT microbenchmarks: one small assembly each, run under the runtime this
# build produces.
#
# These are timing programs, not correctness tests -- none of them has an
# expected-output file, so all a run can tell you is that the program finished
# without faulting. That is still worth having as a CTest label, because these
# shapes (inlining, constant folding, register allocation, cmov selection) are
# exactly the ones a codegen change breaks. They carry `benchmark` and `slow`
# so they stay out of the default run.

if(NOT MONO_ENABLE_BENCHMARKS OR NOT MONO_ENABLE_MCS_BUILD
   OR NOT MONO_ENABLE_EXECUTABLES)
  return()
endif()

set(_src      "${CMAKE_CURRENT_SOURCE_DIR}")
set(_bin      "${CMAKE_CURRENT_BINARY_DIR}")
set(_buildcls "${MONO_MCS_LIBDIR}/build")
set(_wrapper  "${MONO_RUNTIME_WRAPPER}")

mono_tools_runtime(_host MONO_PATH "${_buildcls}" AOT_PATH "${_buildcls}")
set(_csc   ${_host} "${MONO_CSC}" -debug:portable -noconfig -nologo
           -nowarn:0162 -nowarn:0168 -nowarn:0219 -nowarn:0414 -nowarn:0649)
# /quiet drops the per-file "Assembling ..." banner and the "Operation completed
# successfully" line.  Warnings and errors print in both conditions.
set(_ilasm ${_host} "${_buildcls}/ilasm.exe" /quiet)

set(_cs_benchmarks
  fib life castclass
  cmov1 cmov2 cmov3 cmov4 cmov5
  commute isinst sbperf1 sbperf2 iconst-byte
  inline1 inline2 inline3 inline4 inline5 inline6 inline-readonly
  max-min muldiv loops initlocals logic switch ctor-bench
  readonly readonly-byte-array readonly-inst readonly-vt
  regalloc regalloc-2
  math boxtest valuetype-hash-equals vt2)

# IL benchmarks, assembled rather than compiled.
set(_il_benchmarks bulkcpy)

set(_benchmark_exes "")

foreach(_b IN LISTS _cs_benchmarks)
  add_custom_command(
    OUTPUT "${_bin}/${_b}.exe"
    COMMAND ${_csc} "-out:${_bin}/${_b}.exe" "${_src}/${_b}.cs"
    DEPENDS "${_src}/${_b}.cs" mcs
    COMMENT "CSC ${_b}.exe"
    VERBATIM)
  list(APPEND _benchmark_exes "${_bin}/${_b}.exe")
endforeach()

foreach(_b IN LISTS _il_benchmarks)
  add_custom_command(
    OUTPUT "${_bin}/${_b}.exe"
    COMMAND ${_ilasm} "${_src}/${_b}.il" "/OUTPUT=${_bin}/${_b}.exe"
    DEPENDS "${_src}/${_b}.il" mcs
    COMMENT "ILASM ${_b}.exe"
    VERBATIM)
  list(APPEND _benchmark_exes "${_bin}/${_b}.exe")
endforeach()

add_custom_target(benchmarks ALL DEPENDS ${_benchmark_exes})
add_dependencies(benchmarks mono-${MONO_DEFAULT_GC_SUFFIX})

# -O=all is what the old runner passed, and it is the interesting setting: it
# is the one that exercises the optimisation passes these programs were written
# to stress.
foreach(_b IN LISTS _cs_benchmarks _il_benchmarks)
  add_test(NAME "benchmark-${_b}"
           COMMAND "${_wrapper}" -O=all "${_bin}/${_b}.exe")
  set_tests_properties("benchmark-${_b}" PROPERTIES
    LABELS "benchmark;slow"
    TIMEOUT 600
    ENVIRONMENT "MONO_PATH=${MONO_MCS_LIBDIR}/${MONO_DEFAULT_PROFILE}")
endforeach()
