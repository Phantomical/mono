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
set(_class    "${MONO_MCS_LIBDIR}/${MONO_DEFAULT_PROFILE}")
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

# Mono.Simd and System.Numerics.Vector4 aren't in the default reference set,
# so these two compile outside the generic loop above.
# mono/tests/CMakeLists.txt sets simd-semantics.exe apart from its own loop
# the same way.
add_custom_command(
  OUTPUT "${_bin}/simd-matmul.exe"
  COMMAND ${_csc} "-out:${_bin}/simd-matmul.exe" "${_src}/simd-matmul.cs"
          "-r:${_class}/Mono.Simd.dll" -nowarn:0618
  DEPENDS "${_src}/simd-matmul.cs" mcs
  COMMENT "CSC simd-matmul.exe"
  VERBATIM)
add_custom_command(
  OUTPUT "${_bin}/simd-nbody.exe"
  COMMAND ${_csc} "-out:${_bin}/simd-nbody.exe" "${_src}/simd-nbody.cs"
          "-r:${_class}/System.Numerics.dll"
  DEPENDS "${_src}/simd-nbody.cs" mcs
  COMMENT "CSC simd-nbody.exe"
  VERBATIM)
set(_simd_benchmarks simd-matmul simd-nbody)
list(APPEND _benchmark_exes "${_bin}/simd-matmul.exe" "${_bin}/simd-nbody.exe")

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

# Both kernels promote out of tier 0 during the warmup phase. Pinning tier 2
# off keeps the measured phase at tier 1, where the SIMD rows fold into their
# caller (CLAUDE.md, -mono-simd). At tier 2, O3 recovers the same code from
# the managed fallback body, so a regression in the rows would not show up
# there.
foreach(_b IN LISTS _simd_benchmarks)
  add_test(NAME "benchmark-${_b}"
           COMMAND "${_wrapper}" -O=all "${_bin}/${_b}.exe")
  set_tests_properties("benchmark-${_b}" PROPERTIES
    LABELS "benchmark;slow"
    TIMEOUT 600
    ENVIRONMENT "MONO_PATH=${MONO_MCS_LIBDIR}/${MONO_DEFAULT_PROFILE};MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2=0")
endforeach()
