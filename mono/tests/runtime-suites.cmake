# CTest wiring for the runtime corpus.
#
# `check-local` in the automake build was a shell loop that ran fifteen
# sub-makes and OR'ed their exit codes together. Each of those is a CTest test
# here, so a failure names the suite that broke and the rest still run.
#
# Labels. `ctest` with no arguments runs the fast set; everything heavy is
# behind a label so the inner loop stays short. See the `check` target.
#   runtime   the ~700-program corpus and the one-off suites
#   gshared   generic sharing, over four optimization sets
#   sgen      the SGen collector matrix
#   interp    the whole corpus again under the interpreter
#   slow      minutes-long single tests
#   stress    long-running stress tests
#   fixture   builds the managed inputs; pulled in automatically when needed

set(_class_dir "${_class}")

include(ProcessorCount)
ProcessorCount(_mono_nproc)
if(_mono_nproc EQUAL 0)
  set(_mono_nproc 1)
endif()

# test-runner.exe is itself managed code, so it runs on the build profile.
set(_runner
  "${CMAKE_COMMAND}" -E env "MONO_PATH=${_buildcls}" "${_wrapper}" --debug
  "${_bin}/test-runner.exe"
  --config "${_bin}/tests-config"
  --runtime "${_wrapper}"
  --mono-path "${_class_dir}")

string(REPLACE ";" " " _disabled_arg "${MONO_TESTS_DISABLED}")

# Building the corpus is a fixture: CTest cannot build, and ~700 assemblies is
# too much to put in `all`.
add_test(NAME runtime-corpora
         COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target runtime-corpora)
set_tests_properties(runtime-corpora PROPERTIES
  FIXTURES_SETUP runtime_corpora
  LABELS fixture
  TIMEOUT 5400)

# ---------------------------------------------------------------------------
# mono_runtime_suite(<name> TESTS ... [LABEL x] [RUNTIME_ARGS s] [ENV ...]
#                    [OPT_SETS s] [TIMEOUT n] [JOBS n])
# ---------------------------------------------------------------------------
function(mono_runtime_suite name)
  cmake_parse_arguments(ARG "" "LABEL;RUNTIME_ARGS;OPT_SETS;TIMEOUT;JOBS" "TESTS;ENV" ${ARGN})
  if(NOT ARG_TESTS)
    return()
  endif()
  if(NOT ARG_LABEL)
    set(ARG_LABEL runtime)
  endif()
  if(NOT ARG_TIMEOUT)
    set(ARG_TIMEOUT 300)
  endif()
  if(NOT ARG_JOBS)
    set(ARG_JOBS a)
  endif()

  set(_extra "")
  if(ARG_RUNTIME_ARGS)
    list(APPEND _extra --runtime-args "${ARG_RUNTIME_ARGS}")
  endif()
  if(ARG_OPT_SETS)
    list(APPEND _extra --opt-sets "${ARG_OPT_SETS}")
  endif()

  set(_cmd ${_runner})
  if(ARG_ENV)
    set(_cmd "${CMAKE_COMMAND}" -E env ${ARG_ENV} ${_runner})
  endif()

  add_test(NAME ${name}
           COMMAND ${_cmd} -j ${ARG_JOBS}
                   --testsuite-name "${name}"
                   --timeout ${ARG_TIMEOUT}
                   --disabled "${_disabled_arg}"
                   ${_extra}
                   ${ARG_TESTS}
           WORKING_DIRECTORY "${_bin}")
  # Each of these drives test-runner.exe with -j a, so it already uses the
  # whole machine; telling CTest that stops `ctest -j` from stacking two of
  # them and thrashing.
  set_tests_properties(${name} PROPERTIES
    LABELS "${ARG_LABEL}"
    PROCESSORS ${_mono_nproc}
    FIXTURES_REQUIRED runtime_corpora
    TIMEOUT 3600)
endfunction()

# Turn a source list into the .exe list the runner takes, minus the disabled.
function(_mono_exe_list out)
  set(_r "")
  foreach(_s IN LISTS ARGN)
    string(REGEX REPLACE "\\.(cs|il)$" ".exe" _e "${_s}")
    if(NOT _e IN_LIST MONO_TESTS_DISABLED)
      list(APPEND _r "${_e}")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES _r)
  set(${out} "${_r}" PARENT_SCOPE)
endfunction()

_mono_exe_list(_regular ${MONO_TESTS_CS_SRC} ${MONO_TESTS_IL_SRC} ${MONO_TESTS_BENCH_SRC})
_mono_exe_list(_gshared ${MONO_TESTS_GSHARED_SRC})
_mono_exe_list(_stress  ${MONO_TESTS_STRESS_SRC})
_mono_exe_list(_stress_process ${MONO_TESTS_STRESS_PROCESS_SRC})
_mono_exe_list(_sgen_regular   ${MONO_TESTS_SGEN_REGULAR_SRC})
_mono_exe_list(_sgen_toggleref ${MONO_TESTS_SGEN_TOGGLEREF_SRC})
_mono_exe_list(_sgen_bridge    ${MONO_TESTS_SGEN_BRIDGE_SRC})
_mono_exe_list(_sgen_bridge2   ${MONO_TESTS_SGEN_BRIDGE2_SRC})
_mono_exe_list(_sgen_bridge3   ${MONO_TESTS_SGEN_BRIDGE3_SRC})

# Tailcall: the compile list minus the ones that build but do not pass.
_mono_exe_list(_tailcall_all ${MONO_TESTS_TAILCALL_CS_SRC} ${MONO_TESTS_TAILCALL_IL_SRC})
set(_tailcall ${_tailcall_all})
list(REMOVE_ITEM _tailcall
     ${MONO_TESTS_TAILCALL_DISABLED_COMPILE} ${MONO_TESTS_TAILCALL_DISABLED_RUN})

# ---------------------------------------------------------------------------
# The suites
# ---------------------------------------------------------------------------
mono_runtime_suite(runtime TESTS ${_regular})

# MONO_DEBUG=test-tailcall-require turns "the JIT declined to emit a tail call"
# into a failure, which is the whole point of this suite. --compile-all makes
# that cover every method in the assembly rather than only the ones the test
# happens to reach.
mono_runtime_suite(runtime-tailcall TESTS ${_tailcall}
                   ENV "MONO_DEBUG=test-tailcall-require"
                   RUNTIME_ARGS "--compile-all")

mono_runtime_suite(gshared LABEL gshared TESTS ${_gshared}
                   OPT_SETS "gshared gshared,shared gshared,-inline gshared,-inline,shared")

if(MONO_ENABLE_INTERPRETER)
  set(_interp ${_regular})
  list(REMOVE_ITEM _interp ${MONO_TESTS_INTERP_DISABLED})
  # The same ~700 programs again on a much slower engine. Useful, but not on
  # every edit, so it gets a label of its own rather than sitting in `runtime`.
  mono_runtime_suite(runtime-interp LABEL interp TESTS ${_interp}
                     RUNTIME_ARGS "--interpreter")
endif()

mono_runtime_suite(runtime-stress LABEL stress TESTS ${_stress} TIMEOUT 900)
mono_runtime_suite(runtime-process-stress LABEL stress TESTS ${_stress_process} TIMEOUT 900)

# --- the SGen matrix ---------------------------------------------------------
# Each collector configuration is its own test, so a failure names the mode
# rather than just "sgen". The argument strings are verbatim from the automake
# recipes: the collector is selected on the command line, not through
# MONO_GC_PARAMS, and the toggleref and bridge suites need their test hooks
# (`toggleref-test`, `--gc-debug=bridge=...`) switched on or the behaviour they
# check never happens.
function(_mono_sgen_suite name tests args)
  mono_runtime_suite(${name} LABEL sgen TESTS ${${tests}}
                     RUNTIME_ARGS "${args}" TIMEOUT 900)
endfunction()

_mono_sgen_suite(sgen-regular-ms-simple _sgen_regular
                 "--gc=sgen --gc-debug= --gc-params=major=marksweep,minor=simple")
_mono_sgen_suite(sgen-regular-ms-conc-simple _sgen_regular
                 "--gc=sgen --gc-debug= --gc-params=major=marksweep-conc,minor=simple")
_mono_sgen_suite(sgen-regular-ms-conc-par-simple _sgen_regular
                 "--gc=sgen --gc-debug= --gc-params=major=marksweep-conc-par,minor=simple")
_mono_sgen_suite(sgen-regular-ms-conc-split _sgen_regular
                 "--gc=sgen --gc-debug= --gc-params=major=marksweep-conc,minor=split")
_mono_sgen_suite(sgen-regular-ms-conc-split-95-clear-at-gc _sgen_regular
                 "--gc=sgen --gc-debug=clear-at-gc --gc-params=major=marksweep-conc,minor=split,alloc-ratio=95")
_mono_sgen_suite(sgen-regular-ms-conc-par-simple-par-dyn _sgen_regular
                 "--gc=sgen --gc-debug= --gc-params=major=marksweep-conc-par,minor=simple-par,dynamic-nursery")
_mono_sgen_suite(sgen-regular-ms-conc-par-simple-par-512k _sgen_regular
                 "--gc=sgen --gc-debug= --gc-params=major=marksweep-conc-par,minor=simple-par,nursery-size=512k")
_mono_sgen_suite(sgen-regular-ms-conc-par-simple-par-32m _sgen_regular
                 "--gc=sgen --gc-debug= --gc-params=major=marksweep-conc-par,minor=simple-par,nursery-size=32m")
_mono_sgen_suite(sgen-regular-ms-conc-par-simple-par-dyn-clear-at-gc _sgen_regular
                 "--gc=sgen --gc-debug=clear-at-gc --gc-params=major=marksweep-conc-par,minor=simple-par,dynamic-nursery")
_mono_sgen_suite(sgen-toggleref-ms-simple _sgen_toggleref
                 "--gc=sgen --gc-debug= --gc-params=toggleref-test,major=marksweep,minor=simple")
_mono_sgen_suite(sgen-toggleref-ms-conc-simple _sgen_toggleref
                 "--gc=sgen --gc-debug= --gc-params=toggleref-test,major=marksweep-conc,minor=simple")
_mono_sgen_suite(sgen-toggleref-ms-conc-par-simple _sgen_toggleref
                 "--gc=sgen --gc-debug= --gc-params=toggleref-test,major=marksweep-conc-par,minor=simple")
_mono_sgen_suite(sgen-toggleref-ms-conc-split _sgen_toggleref
                 "--gc=sgen --gc-debug= --gc-params=toggleref-test,major=marksweep-conc,minor=split")
_mono_sgen_suite(sgen-toggleref-ms-conc-split-95-clear-at-gc _sgen_toggleref
                 "--gc=sgen --gc-debug=clear-at-gc --gc-params=toggleref-test,major=marksweep-conc,minor=split,alloc-ratio=95")
_mono_sgen_suite(sgen-toggleref-ms-conc-par-simple-par-dyn _sgen_toggleref
                 "--gc=sgen --gc-debug= --gc-params=toggleref-test,major=marksweep-conc-par,minor=simple-par,dynamic-nursery")
_mono_sgen_suite(sgen-toggleref-ms-conc-par-simple-par-512k _sgen_toggleref
                 "--gc=sgen --gc-debug= --gc-params=toggleref-test,major=marksweep-conc-par,minor=simple-par,nursery-size=512k")
_mono_sgen_suite(sgen-toggleref-ms-conc-par-simple-par-32m _sgen_toggleref
                 "--gc=sgen --gc-debug= --gc-params=toggleref-test,major=marksweep-conc-par,minor=simple-par,nursery-size=32m")
_mono_sgen_suite(sgen-toggleref-ms-conc-par-simple-par-dyn-clear-at-gc _sgen_toggleref
                 "--gc=sgen --gc-debug=clear-at-gc --gc-params=toggleref-test,major=marksweep-conc-par,minor=simple-par,dynamic-nursery")
_mono_sgen_suite(sgen-bridge-ms-simple-tarjan-bridge _sgen_bridge
                 "--gc=sgen --gc-debug=bridge=Bridge --gc-params=major=marksweep,minor=simple,bridge-implementation=tarjan")
_mono_sgen_suite(sgen-bridge-ms-conc-simple-tarjan-bridge _sgen_bridge
                 "--gc=sgen --gc-debug=bridge=Bridge --gc-params=major=marksweep-conc,minor=simple,bridge-implementation=tarjan")
_mono_sgen_suite(sgen-bridge-ms-conc-split-tarjan-bridge _sgen_bridge
                 "--gc=sgen --gc-debug=bridge=Bridge --gc-params=major=marksweep-conc,minor=split,bridge-implementation=tarjan")
_mono_sgen_suite(sgen-bridge-ms-conc-simple-new-bridge _sgen_bridge
                 "--gc=sgen --gc-debug=bridge=Bridge --gc-params=major=marksweep-conc,minor=simple,bridge-implementation=new")
_mono_sgen_suite(sgen-bridge-ms-conc-simple-old-bridge _sgen_bridge
                 "--gc=sgen --gc-debug=bridge=Bridge --gc-params=major=marksweep-conc,minor=simple,bridge-implementation=old")
_mono_sgen_suite(sgen-bridge-ms-conc-par-simple-par-dyn-tarjan-bridge _sgen_bridge
                 "--gc=sgen --gc-debug=bridge=Bridge --gc-params=major=marksweep-conc-par,minor=simple-par,dynamic-nursery,bridge-implementation=tarjan")
_mono_sgen_suite(sgen-bridge-ms-conc-par-simple-par-512k-tarjan-bridge _sgen_bridge
                 "--gc=sgen --gc-debug=bridge=Bridge --gc-params=major=marksweep-conc-par,minor=simple-par,nursery-size=512k,bridge-implementation=tarjan")
_mono_sgen_suite(sgen-bridge-ms-conc-par-simple-par-32m-tarjan-bridge _sgen_bridge
                 "--gc=sgen --gc-debug=bridge=Bridge --gc-params=major=marksweep-conc-par,minor=simple-par,nursery-size=32m,bridge-implementation=tarjan")
_mono_sgen_suite(sgen-bridge2-ms-simple-tarjan-bridge _sgen_bridge2
                 "--gc=sgen --gc-debug=bridge=2Bridge --gc-params=major=marksweep,minor=simple,bridge-implementation=tarjan")
_mono_sgen_suite(sgen-bridge2-ms-conc-simple-tarjan-bridge _sgen_bridge2
                 "--gc=sgen --gc-debug=bridge=2Bridge --gc-params=major=marksweep-conc,minor=simple,bridge-implementation=tarjan")
_mono_sgen_suite(sgen-bridge2-ms-conc-split-tarjan-bridge _sgen_bridge2
                 "--gc=sgen --gc-debug=bridge=2Bridge --gc-params=major=marksweep-conc,minor=split,bridge-implementation=tarjan")
_mono_sgen_suite(sgen-bridge2-ms-conc-simple-new-bridge _sgen_bridge2
                 "--gc=sgen --gc-debug=bridge=2Bridge --gc-params=major=marksweep-conc,minor=simple,bridge-implementation=new")
_mono_sgen_suite(sgen-bridge2-ms-conc-simple-old-bridge _sgen_bridge2
                 "--gc=sgen --gc-debug=bridge=2Bridge --gc-params=major=marksweep-conc,minor=simple,bridge-implementation=old")
_mono_sgen_suite(sgen-bridge2-ms-conc-par-simple-par-dyn-tarjan-bridge _sgen_bridge2
                 "--gc=sgen --gc-debug=bridge=2Bridge --gc-params=major=marksweep-conc-par,minor=simple-par,dynamic-nursery,bridge-implementation=tarjan")
_mono_sgen_suite(sgen-bridge2-ms-conc-par-simple-par-512k-tarjan-bridge _sgen_bridge2
                 "--gc=sgen --gc-debug=bridge=2Bridge --gc-params=major=marksweep-conc-par,minor=simple-par,nursery-size=512k,bridge-implementation=tarjan")
_mono_sgen_suite(sgen-bridge2-ms-conc-par-simple-par-32m-tarjan-bridge _sgen_bridge2
                 "--gc=sgen --gc-debug=bridge=2Bridge --gc-params=major=marksweep-conc-par,minor=simple-par,nursery-size=32m,bridge-implementation=tarjan")
_mono_sgen_suite(sgen-bridge3-ms-simple-tarjan-bridge _sgen_bridge3
                 "--gc=sgen --gc-debug=bridge=3Bridge --gc-params=major=marksweep,minor=simple,bridge-implementation=tarjan")
_mono_sgen_suite(sgen-bridge3-ms-conc-simple-tarjan-bridge _sgen_bridge3
                 "--gc=sgen --gc-debug=bridge=3Bridge --gc-params=major=marksweep-conc,minor=simple,bridge-implementation=tarjan")
_mono_sgen_suite(sgen-bridge3-ms-conc-split-tarjan-bridge _sgen_bridge3
                 "--gc=sgen --gc-debug=bridge=3Bridge --gc-params=major=marksweep-conc,minor=split,bridge-implementation=tarjan")
_mono_sgen_suite(sgen-bridge3-ms-conc-simple-new-bridge _sgen_bridge3
                 "--gc=sgen --gc-debug=bridge=3Bridge --gc-params=major=marksweep-conc,minor=simple,bridge-implementation=new")
_mono_sgen_suite(sgen-bridge3-ms-conc-simple-old-bridge _sgen_bridge3
                 "--gc=sgen --gc-debug=bridge=3Bridge --gc-params=major=marksweep-conc,minor=simple,bridge-implementation=old")
_mono_sgen_suite(sgen-bridge3-ms-conc-par-simple-par-dyn-tarjan-bridge _sgen_bridge3
                 "--gc=sgen --gc-debug=bridge=3Bridge --gc-params=major=marksweep-conc-par,minor=simple-par,dynamic-nursery,bridge-implementation=tarjan")
_mono_sgen_suite(sgen-bridge3-ms-conc-par-simple-par-512k-tarjan-bridge _sgen_bridge3
                 "--gc=sgen --gc-debug=bridge=3Bridge --gc-params=major=marksweep-conc-par,minor=simple-par,nursery-size=512k,bridge-implementation=tarjan")
_mono_sgen_suite(sgen-bridge3-ms-conc-par-simple-par-32m-tarjan-bridge _sgen_bridge3
                 "--gc=sgen --gc-debug=bridge=3Bridge --gc-params=major=marksweep-conc-par,minor=simple-par,nursery-size=32m,bridge-implementation=tarjan")

# --- one-off suites ----------------------------------------------------------
# These are not test-runner corpora: each is a single program whose exit code
# or output is the result.
function(mono_runtime_check name)
  cmake_parse_arguments(ARG "" "TIMEOUT" "COMMAND;ENV;DEPENDS" ${ARGN})
  if(NOT ARG_TIMEOUT)
    set(ARG_TIMEOUT 300)
  endif()
  set(_pfx "${CMAKE_COMMAND}" -E env "MONO_PATH=${_class_dir}")
  if(ARG_ENV)
    set(_pfx "${CMAKE_COMMAND}" -E env "MONO_PATH=${_class_dir}" ${ARG_ENV})
  endif()
  add_test(NAME ${name} COMMAND ${_pfx} ${ARG_COMMAND}
           WORKING_DIRECTORY "${_bin}")
  set_tests_properties(${name} PROPERTIES
    LABELS runtime FIXTURES_REQUIRED runtime_corpora TIMEOUT ${ARG_TIMEOUT})
endfunction()

mono_runtime_check(runtime-type-load
  COMMAND "${_wrapper}" load-exceptions.exe)
mono_runtime_check(runtime-multi-netmodule
  COMMAND "${_wrapper}" test-multi-netmodule-4-exe.exe)
mono_runtime_check(runtime-cattr-type-load
  COMMAND "${_wrapper}" custom-attr-errors.exe)
mono_runtime_check(runtime-reflection-load-with-context
  COMMAND "${_wrapper}" reflection-load-with-context.exe)
mono_runtime_check(runtime-iomap-regression
  COMMAND "${_wrapper}" exists.exe ENV "MONO_IOMAP=all")
# The four unhandled-exception suites `check-local` ran: the exit code the
# runtime should produce for an unhandled exception, with and without a managed
# AppDomain.UnhandledException handler installed.
#
# (There is also an `unhandled-exception-test-runner.2.exe` driver in the tree.
# automake had it behind a `test-unhandled-exception` target that `check` never
# invoked, and it currently reports a failing configuration, so it is not wired
# up here either.)
_mono_exe_list(_unhandled_1   ${MONO_TESTS_UNHANDLED_EXCEPTION_1_SRC})
_mono_exe_list(_unhandled_255 ${MONO_TESTS_UNHANDLED_EXCEPTION_255_SRC})

# TEST_UNHANDLED_EXCEPTION_HANDLER makes the test subscribe to
# AppDomain.UnhandledException, so setting it is the *with*-handler case.
# automake had these two target names the other way round; the names here
# follow the source.
function(_mono_unhandled_suite code handler)
  if(handler)
    set(_name "runtime-unhandled-exception-${code}-with-managed-handler")
    set(_env "TEST_UNHANDLED_EXCEPTION_HANDLER=1")
  else()
    set(_name "runtime-unhandled-exception-${code}-without-managed-handler")
    set(_env "")
  endif()
  add_test(NAME ${_name}
           COMMAND "${CMAKE_COMMAND}" -E env ${_env} ${_runner}
                   -j a --testsuite-name "${_name}"
                   --disabled "${_disabled_arg}"
                   --expected-exit-code ${code}
                   ${ARGN}
           WORKING_DIRECTORY "${_bin}")
  set_tests_properties(${_name} PROPERTIES
    LABELS runtime FIXTURES_REQUIRED runtime_corpora TIMEOUT 900)
endfunction()

_mono_unhandled_suite(1   OFF ${_unhandled_1})
_mono_unhandled_suite(1   ON  ${_unhandled_1})
_mono_unhandled_suite(255 OFF ${_unhandled_255})
_mono_unhandled_suite(255 ON  ${_unhandled_255})

# MONO_ENV_OPTIONS has to reach the runtime before it parses its own argv.
add_test(NAME runtime-env-options
         COMMAND "${CMAKE_COMMAND}" -E env "MONO_PATH=${_class_dir}"
                 "MONO_ENV_OPTIONS=--version" "${_wrapper}" array-init.exe
         WORKING_DIRECTORY "${_bin}")
set_tests_properties(runtime-env-options PROPERTIES
  LABELS runtime FIXTURES_REQUIRED runtime_corpora
  PASS_REGULAR_EXPRESSION "Architecture:")

# eglib's symbols are remapped to monoeg_* so that a runtime linked into a host
# that already has glib does not collide with it. Anything still exported as a
# bare g_* is a missed entry in eglib-remap.h.
find_program(MONO_NM nm)
if(MONO_NM)
  add_test(NAME runtime-eglib-remap
           COMMAND "${CMAKE_COMMAND}"
                   "-DNM=${MONO_NM}"
                   "-DBINARY=${CMAKE_BINARY_DIR}/mono/mini/mono-${MONO_DEFAULT_GC_SUFFIX}"
                   -P "${CMAKE_SOURCE_DIR}/cmake/MonoCheckEglibRemap.cmake")
  set_tests_properties(runtime-eglib-remap PROPERTIES LABELS runtime)
endif()

mono_runtime_suite(runtime-internalsvisibleto
  TESTS internalsvisibleto-runtimetest.exe internalsvisibleto-compilertest.exe
        internalsvisibleto-runtimetest-sign2048.exe
        internalsvisibleto-compilertest-sign2048.exe)

mono_runtime_check(runtime-pedump
  COMMAND "${CMAKE_BINARY_DIR}/tools/pedump/pedump" --verify code,metadata
          "${_class_dir}/mscorlib.dll")
