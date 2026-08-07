# CTest wiring for the runtime corpus.
#
# Every program in the corpus is its own CTest test, named `<suite>/<program>`
# -- so `ctest -R runtime/bug-18026` runs exactly one of them, `--rerun-failed`
# re-runs only what broke, and a suite that covers the same programs several
# ways (the four gshared optimization sets, the thirty-odd SGen collector
# configurations) reports which configuration failed rather than just that one
# did. Each also runs on every collector that was built, `...@sgen` and
# `...@boehm`. The corresponding cost is that `ctest -N` lists ~3400 tests.
#
# Labels. `ctest` with no arguments runs the fast set; everything heavy is
# behind a label so the inner loop stays short. See the `check` target.
#   runtime   the ~700-program corpus and the one-off suites
#   gshared   generic sharing, over four optimization sets
#   sgen      the SGen collector matrix
#   interp    the whole corpus again under the interpreter
#   slow      minutes-long single tests
#   stress    long-running stress tests

set(_class_dir "${_class}")
set(_run_test "${CMAKE_SOURCE_DIR}/cmake/MonoRunTest.cmake")

# Which collector each suite runs on. The corpus is collector-agnostic, so it
# runs on every runtime that was built -- mono-wrapper picks the binary out of
# MONO_EXECUTABLE, so this costs an environment variable and a name suffix.
#
# The SGen matrix is the exception and asks for `GC sgen`. mono-boehm accepts
# --gc=sgen and --gc-params, ignores them, and exits 0, so running those suites
# on it would add a hundred and sixty passing tests that assert nothing.
set(_mono_gcs "")
if(MONO_ENABLE_SGEN)
  list(APPEND _mono_gcs sgen)
endif()
if(MONO_ENABLE_BOEHM)
  list(APPEND _mono_gcs boehm)
endif()

# The collector a test ran on lives in its name (`...@sgen`, `...@boehm`) and
# not in a label. `ctest -L` matches labels as a regex, so a `gc-sgen` label
# would also be picked up by `-L sgen` -- which already means the collector
# matrix, a different set. Select a collector with `-R '@boehm$'`, and combine
# it with `-L` as usual.
function(_mono_gc_env out_env gc)
  set(${out_env} "MONO_EXECUTABLE=${CMAKE_BINARY_DIR}/mono/mini/mono-${gc}" PARENT_SCOPE)
endfunction()

# Tests that saturate the machine on their own -- they scale their own thread
# count off Environment.ProcessorCount, or hammer the thread pool. CTest assumes
# one core per test, packs `-j` of them alongside everything else, and starves
# them: appdomain-threadpool-unload runs in 3s and has twice timed out at 300s
# in a full `check-all`. Claiming several cores each keeps that from happening.
set(_mono_parallel_hungry
  appdomain-threadpool-unload.exe
  process-unref-race.exe
  namedmutex-destroy-race.exe
  pinvoke-detach-1.exe
  bug-18026.exe
)

# ---------------------------------------------------------------------------
# mono_runtime_suite(<name> TESTS ... [LABEL x] [RUNTIME_ARGS s] [ENV ...]
#                    [OPT_SETS s] [TIMEOUT n] [EXPECT n] [WORKDIR d]
#                    [PROCESSORS n] [GC ...] [SKIP_BOEHM ...]
#                    [LONG ... [LONG_TIMEOUT n]])
#
# One CTest test per program, named `<suite>/<program>` -- and per optimization
# set on top of that, `<suite>/<program>:<opt-set>`, since those are separate
# runs that fail separately.
#
# This is deliberately not test-runner.exe driving a whole list. CTest's own
# scheduler then owns the parallelism, `ctest -R` addresses one program, and
# `--rerun-failed` re-runs the six that broke rather than all seven hundred.
# What test-runner did per child is small enough to reproduce inline: set
# MONO_PATH/MONO_CONFIG, hand the runtime `-O=<opt-set>` and the suite's
# runtime arguments, and compare the exit code. The per-test timeout it passed
# down in TEST_DRIVER_TIMEOUT_SEC is still passed down -- tests that pace
# themselves read it through TestTimeout.
# ---------------------------------------------------------------------------
function(mono_runtime_suite name)
  cmake_parse_arguments(ARG "" "LABEL;RUNTIME_ARGS;OPT_SETS;TIMEOUT;EXPECT;WORKDIR;PROCESSORS;LONG_TIMEOUT"
                            "TESTS;ENV;GC;SKIP_BOEHM;LONG" ${ARGN})
  if(NOT ARG_TESTS)
    return()
  endif()
  if(NOT ARG_GC)
    set(ARG_GC ${_mono_gcs})
  else()
    # A suite may ask for a collector this build did not produce; drop it rather
    # than emitting a test that cannot run.
    set(_want "")
    foreach(_g IN LISTS ARG_GC)
      if(_g IN_LIST _mono_gcs)
        list(APPEND _want "${_g}")
      endif()
    endforeach()
    set(ARG_GC "${_want}")
  endif()
  if(NOT ARG_GC)
    return()
  endif()
  if(NOT ARG_LABEL)
    set(ARG_LABEL runtime)
  endif()
  if(NOT ARG_TIMEOUT)
    set(ARG_TIMEOUT 300)
  endif()
  if(NOT ARG_LONG_TIMEOUT)
    set(ARG_LONG_TIMEOUT 900)
  endif()
  if(NOT ARG_EXPECT)
    set(ARG_EXPECT 0)
  endif()
  if(NOT ARG_WORKDIR)
    set(ARG_WORKDIR "${_bin}")
  endif()

  # Both arrive as one space-separated string, the shape the automake recipes
  # used and the shape test-runner.exe parsed.
  set(_rt_args "")
  if(ARG_RUNTIME_ARGS)
    separate_arguments(_rt_args UNIX_COMMAND "${ARG_RUNTIME_ARGS}")
  endif()
  set(_opt_sets "")
  if(ARG_OPT_SETS)
    separate_arguments(_opt_sets UNIX_COMMAND "${ARG_OPT_SETS}")
  endif()
  # `-` stands for "no optimization set", so the loop below still runs once for
  # a suite that asked for none. An empty element cannot say that: CMake does
  # not distinguish a one-empty-element list from an empty one, and an empty
  # list iterates zero times.
  if(NOT _opt_sets)
    set(_opt_sets "-")
  endif()

  foreach(_gc IN LISTS ARG_GC)
    _mono_gc_env(_gc_env "${_gc}")
    # SKIP_BOEHM drops the tests that fail on Boehm from the boehm half only,
    # so the sgen half of the suite still covers them.
    set(_gc_tests ${ARG_TESTS})
    if(_gc STREQUAL "boehm" AND ARG_SKIP_BOEHM)
      list(REMOVE_ITEM _gc_tests ${ARG_SKIP_BOEHM})
    endif()

    foreach(_test IN LISTS _gc_tests)
      string(REGEX REPLACE "\\.exe$" "" _stem "${_test}")

      # LONG names the programs whose work does not fit the suite's budget --
      # not slow by accident, but asking for far more of the runtime than their
      # neighbours do.
      set(_timeout ${ARG_TIMEOUT})
      if(_test IN_LIST ARG_LONG)
        set(_timeout ${ARG_LONG_TIMEOUT})
      endif()
      # A little above what MonoRunTest gives the test, so the SIGQUIT thread
      # dump wins the race and CTest only steps in if that failed too.
      math(EXPR _ctest_timeout "${_timeout} + 60")

      foreach(_opt IN LISTS _opt_sets)
        if(_opt STREQUAL "-")
          set(_tname "${name}/${_stem}")
          set(_oarg "")
        else()
          set(_tname "${name}/${_stem}:${_opt}")
          set(_oarg "-O=${_opt}")
        endif()
        set(_gname "${_tname}@${_gc}")
        add_test(NAME "${_gname}"
                 COMMAND "${CMAKE_COMMAND}" -E env
                         "MONO_PATH=${_class_dir}"
                         "MONO_CONFIG=${_bin}/tests-config"
                         "TEST_DRIVER_TIMEOUT_SEC=${_timeout}"
                         "${_gc_env}"
                         ${ARG_ENV}
                         "${CMAKE_COMMAND}"
                         "-DMONO_TEST_EXPECT=${ARG_EXPECT}"
                         "-DMONO_TEST_TIMEOUT=${_timeout}"
                         -P "${_run_test}" --
                         "${_wrapper}" ${_oarg} ${_rt_args} "${_test}"
                 WORKING_DIRECTORY "${ARG_WORKDIR}")
        set_tests_properties("${_gname}" PROPERTIES
          LABELS "${ARG_LABEL}"
          TIMEOUT ${_ctest_timeout})
        if(ARG_PROCESSORS)
          set_tests_properties("${_gname}" PROPERTIES PROCESSORS ${ARG_PROCESSORS})
        elseif(_test IN_LIST _mono_parallel_hungry)
          set_tests_properties("${_gname}" PROPERTIES PROCESSORS 4)
        endif()
      endforeach()
    endforeach()
  endforeach()
endfunction()

# Turn a source list into the .exe list a suite takes, minus the disabled.
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
# The corpus default is 300s, which the three below have been measured getting
# uncomfortably close to on a machine with its cores busy. Each is slow for a
# reason its neighbours are not, so each gets the long budget rather than the
# whole corpus being loosened to cover them.
#
#   dynamic-method-churn  asks the JIT for 40000 compiles -- 20000 dynamic
#     methods, each with a runtime-invoke wrapper of its own, since a dynamic
#     method cannot share the cached one. That is minutes of LLVM at any
#     per-method cost this backend could plausibly reach; measured at 412s.
#   appdomain-threadpool-unload  unloads 100 domains from a PLINQ query sized
#     to ProcessorCount, each with a thread-pool item spinning in it. It wants
#     the whole machine and gets a share of it, so its cost is set by what else
#     is running: 140s, 227s and 266s across runs, and killed at the 300s mark
#     on the boehm half of a full sweep.
#   appdomain-unload  creates and unloads domains with deliberately slow
#     finalizers and a 10s BeginInvoke still in flight, so most of its time is
#     spent waiting rather than running -- 171s to 247s measured, which is not
#     margin enough to leave at 300s.
mono_runtime_suite(runtime TESTS ${_regular}
                   SKIP_BOEHM ${MONO_TESTS_BOEHM_DISABLED}
                   LONG dynamic-method-churn.exe
                        appdomain-unload.exe
                        appdomain-threadpool-unload.exe)

# CoreCLR's tailcall corpus, run like any other program: several of these
# recurse deeply enough that a missed tail call is a stack overflow rather than
# a subtle difference, so running them is the check.
mono_runtime_suite(runtime-tailcall TESTS ${_tailcall})

mono_runtime_suite(gshared LABEL gshared TESTS ${_gshared})

if(MONO_ENABLE_INTERPRETER)
  set(_interp ${_regular})
  list(REMOVE_ITEM _interp ${MONO_TESTS_INTERP_DISABLED})
  # The same ~700 programs again on a much slower engine. Useful, but not on
  # every edit, so it gets a label of its own rather than sitting in `runtime`.
  # Only appdomain-threadpool-unload needs the long budget here. The other two
  # the JIT gives it to are cheap under the interpreter -- their cost is
  # compilation, and there is none -- but this one's cost is domain unloads
  # against a saturated machine, which the interpreter does not make faster.
  mono_runtime_suite(runtime-interp LABEL interp TESTS ${_interp}
                     RUNTIME_ARGS "--interpreter"
                     SKIP_BOEHM ${MONO_TESTS_BOEHM_DISABLED}
                     LONG appdomain-threadpool-unload.exe)

  # Both engines in one process, which neither of the suites above covers:
  # Callee's methods interpret while its caller compiles, so every call in it
  # crosses the boundary. Argument placement there is settled by the compiler
  # rather than fixed by an ABI document, and being wrong about it corrupts
  # values rather than crashing - so it wants a test that reads them back.
  mono_runtime_suite(runtime-interp-entries LABEL interp
                     TESTS interp-entries.exe
                     RUNTIME_ARGS "--interp-tier0=Callee")

  # The other direction: Interpreted's methods run in the interpreter while
  # their callees compile, so every call in Run () leaves the interpreter for
  # native code. Nothing else covers that crossing -- the suites above either
  # compile everything or interpret everything, and under a bare --interp-tier0
  # every callee is interpreted too.
  mono_runtime_suite(runtime-interp-calls-compiled LABEL interp
                     TESTS interp-calls-compiled.exe
                     RUNTIME_ARGS "--interp-tier0=Interpreted")

  # `--interp=jit=<class>` compiles the named class and interprets the rest,
  # which is the only way today to get a compiled frame and an interpreted one
  # into the same process. Values the two engines hand each other -- a method
  # pointer above all -- only have to agree here.
  mono_runtime_suite(runtime-interp-jit LABEL interp
                     TESTS interp-jit-delegate.exe
                     RUNTIME_ARGS "--interpreter --interp=jit=Creator")
endif()

# domain-stress runs the appdomain create/unload loop for a fixed iteration
# count rather than a fixed duration, so its wall time is whatever the machine
# gives it. A full run has been measured at 816s of the suite's 900s, and it has
# been killed at the 900s mark on both collectors -- which reports as a plain
# failure, not a timeout, because it is the driver that does the killing.
mono_runtime_suite(runtime-stress LABEL stress TESTS ${_stress} TIMEOUT 900
                   LONG domain-stress.exe LONG_TIMEOUT 1800)
mono_runtime_suite(runtime-process-stress LABEL stress TESTS ${_stress_process} TIMEOUT 900)

# --- the SGen matrix ---------------------------------------------------------
# Each collector configuration is its own test, so a failure names the mode
# rather than just "sgen". The argument strings are verbatim from the automake
# recipes: the collector is selected on the command line, not through
# MONO_GC_PARAMS, and the toggleref and bridge suites need their test hooks
# (`toggleref-test`, `--gc-debug=bridge=...`) switched on or the behaviour they
# check never happens.
#
# PROCESSORS: these are GC stress programs, and most of the configurations run a
# concurrent or parallel collector, so one of them is worth several ordinary
# tests to the scheduler. Without the hint CTest packs `-j` of them alongside
# everything else and starves the rest -- `check-all` runs this label next to
# `interp`, where the symptom was an interpreted test that takes 7s timing out
# at 300s.
function(_mono_sgen_suite name tests args)
  mono_runtime_suite(${name} LABEL sgen GC sgen TESTS ${${tests}}
                     RUNTIME_ARGS "${args}" TIMEOUT 900 PROCESSORS 4)
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
# NATIVE is for the ones that do not run managed code on the runtime being
# built -- there is no collector to vary, so they get no @<gc> suffix.
function(mono_runtime_check name)
  cmake_parse_arguments(ARG "NATIVE" "TIMEOUT" "COMMAND;ENV;DEPENDS" ${ARGN})
  if(NOT ARG_TIMEOUT)
    set(ARG_TIMEOUT 300)
  endif()
  if(ARG_NATIVE)
    set(_gcs "")
  else()
    set(_gcs ${_mono_gcs})
  endif()

  foreach(_gc IN LISTS _gcs)
    _mono_gc_env(_gc_env "${_gc}")
    add_test(NAME "${name}@${_gc}"
             COMMAND "${CMAKE_COMMAND}" -E env "MONO_PATH=${_class_dir}"
                     "${_gc_env}" ${ARG_ENV} ${ARG_COMMAND}
             WORKING_DIRECTORY "${_bin}")
    set_tests_properties("${name}@${_gc}" PROPERTIES
      LABELS runtime TIMEOUT ${ARG_TIMEOUT})
  endforeach()

  if(ARG_NATIVE)
    add_test(NAME ${name}
             COMMAND "${CMAKE_COMMAND}" -E env "MONO_PATH=${_class_dir}"
                     ${ARG_ENV} ${ARG_COMMAND}
             WORKING_DIRECTORY "${_bin}")
    set_tests_properties(${name} PROPERTIES
      LABELS runtime TIMEOUT ${ARG_TIMEOUT})
  endif()
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
    mono_runtime_suite(${_name} TESTS ${ARGN} EXPECT ${code}
                       ENV "TEST_UNHANDLED_EXCEPTION_HANDLER=1")
  else()
    set(_name "runtime-unhandled-exception-${code}-without-managed-handler")
    mono_runtime_suite(${_name} TESTS ${ARGN} EXPECT ${code})
  endif()
endfunction()

_mono_unhandled_suite(1   OFF ${_unhandled_1})
_mono_unhandled_suite(1   ON  ${_unhandled_1})
_mono_unhandled_suite(255 OFF ${_unhandled_255})
_mono_unhandled_suite(255 ON  ${_unhandled_255})

# A thread that leaves through pthread_exit drags glibc's forced unwind over
# every JIT'd frame below it. 42 is the exit code the program asks for once it
# has come back out of that.
_mono_exe_list(_forced_unwind ${MONO_TESTS_FORCED_UNWIND_SRC})
mono_runtime_suite(runtime-forced-unwind TESTS ${_forced_unwind} EXPECT 42)

# MONO_ENV_OPTIONS has to reach the runtime before it parses its own argv.
foreach(_gc IN LISTS _mono_gcs)
  _mono_gc_env(_gc_env "${_gc}")
  add_test(NAME "runtime-env-options@${_gc}"
           COMMAND "${CMAKE_COMMAND}" -E env "MONO_PATH=${_class_dir}"
                   "${_gc_env}"
                   "MONO_ENV_OPTIONS=--version" "${_wrapper}" array-init.exe
           WORKING_DIRECTORY "${_bin}")
  set_tests_properties("runtime-env-options@${_gc}" PROPERTIES
    LABELS runtime
    PASS_REGULAR_EXPRESSION "Architecture:")
endforeach()

# eglib's symbols are remapped to monoeg_* so that a runtime linked into a host
# that already has glib does not collide with it. Anything still exported as a
# bare g_* is a missed entry in eglib-remap.h.
# It is a property of each binary that was linked, so check each of them.
find_program(MONO_NM nm)
if(MONO_NM)
  foreach(_gc IN LISTS _mono_gcs)
    add_test(NAME "runtime-eglib-remap@${_gc}"
             COMMAND "${CMAKE_COMMAND}"
                     "-DNM=${MONO_NM}"
                     "-DBINARY=${CMAKE_BINARY_DIR}/mono/mini/mono-${_gc}"
                     -P "${CMAKE_SOURCE_DIR}/cmake/MonoCheckEglibRemap.cmake")
    set_tests_properties("runtime-eglib-remap@${_gc}" PROPERTIES LABELS runtime)
  endforeach()
endif()

mono_runtime_suite(runtime-internalsvisibleto
  TESTS internalsvisibleto-runtimetest.exe internalsvisibleto-compilertest.exe
        internalsvisibleto-runtimetest-sign2048.exe
        internalsvisibleto-compilertest-sign2048.exe)

# valid-only, because corlib is not verifiable and never has been: it is full of
# localloc and native pointers, and Roslyn no longer emits the verifiable
# encodings of the rest. What the check is worth is that none of it is *invalid*.
mono_runtime_check(runtime-pedump NATIVE
  COMMAND "${CMAKE_BINARY_DIR}/tools/pedump/pedump" --verify code,metadata,valid-only
          "${_class_dir}/mscorlib.dll")

# The IL verifier, which --security=validil turns on.
#
# verification-invalid-il.exe calls a method whose body is invalid but which
# both engines happily run, and prints "ran 42" or "rejected <exception>". The
# four arms are the two security settings crossed with the two tiers: the flag
# is what makes the difference, and the tier is what must not.
#
# The tier-0 arms name the invalid method rather than taking every method,
# because a method the interpreter calls from another interpreted method never
# reaches the backend at all. Selecting only the callee keeps its caller
# compiled, so the call goes through the stub the backend published.
function(_mono_verification_check name expect)
  cmake_parse_arguments(ARG "" "" "ARGS;REJECT" ${ARGN})
  foreach(_gc IN LISTS _mono_gcs)
    _mono_gc_env(_gc_env "${_gc}")
    add_test(NAME "${name}@${_gc}"
             COMMAND "${CMAKE_COMMAND}" -E env "MONO_PATH=${_class_dir}"
                     "${_gc_env}" "MONO_LLVM_JIT_TRACE=1"
                     "${_wrapper}" ${ARG_ARGS} verification-invalid-il.exe
             WORKING_DIRECTORY "${_bin}")
    set_tests_properties("${name}@${_gc}" PROPERTIES
      LABELS runtime TIMEOUT 300
      PASS_REGULAR_EXPRESSION "${expect}"
      FAIL_REGULAR_EXPRESSION "${ARG_REJECT}")
  endforeach()
endfunction()

_mono_verification_check(runtime-verification-off "ran 42"
                         REJECT "rejected")
_mono_verification_check(runtime-verification-validil
                         "rejected System.InvalidProgramException"
                         ARGS --security=validil REJECT "ran 42")
_mono_verification_check(runtime-verification-tier0
                         "interpreting Probe:Unverifiable"
                         ARGS --interp-tier0=Probe:Unverifiable REJECT "rejected")
# Rejecting without ever printing the routing line is the placement itself: the
# verdict is reached before the tier is chosen.
_mono_verification_check(runtime-verification-validil-tier0
                         "rejected System.InvalidProgramException"
                         ARGS --security=validil --interp-tier0=Probe:Unverifiable
                         REJECT "ran 42|interpreting Probe:Unverifiable")
