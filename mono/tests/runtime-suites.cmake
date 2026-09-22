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
# Labels. `ctest` with no arguments runs the fast set. Everything heavy is
# behind a label so the inner loop stays short. See the `check` target.
#   runtime   the ~700-program corpus and the one-off suites
#   gshared   generic sharing, over four optimization sets
#   sgen      the SGen collector matrix
#   slow      minutes-long single tests
#   stress    long-running stress tests

set(_class_dir "${_class}")
set(_run_test "${CMAKE_SOURCE_DIR}/cmake/MonoRunTest.cmake")

# Where the per-test temporary directories go. Under the host's temporary root
# rather than the build tree so that TMPDIR still means what the corpus expects
# of it, and keyed by the build directory so two worktrees running the suite at
# once get their own.
string(SHA256 _tmp_key "${CMAKE_BINARY_DIR}")
string(SUBSTRING "${_tmp_key}" 0 12 _tmp_key)
if(WIN32)
  # A path with no drive letter is rooted on whichever drive is current, and
  # mono reads TMPDIR ahead of TEMP. A test that resolves an assembly against
  # Path.GetTempPath () then looks on the wrong volume and does not find it.
  file(TO_CMAKE_PATH "$ENV{TEMP}" _tmp_base)
  if(NOT _tmp_base)
    set(_tmp_base "${CMAKE_BINARY_DIR}/tmp")
  endif()
else()
  set(_tmp_base "/tmp")
endif()
set(_tmp_root "${_tmp_base}/mono-tests-${_tmp_key}")

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

# mono_runtime_suite(<name> TESTS ... [LABEL x] [RUNTIME_ARGS s] [ENV ...]
#                    [OPT_SETS s] [TIMEOUT n] [EXPECT n] [WORKDIR d]
#                    [PROCESSORS n] [GC ...] [SKIP_BOEHM ...] [XFAIL ...]
#                    [LONG ... [LONG_TIMEOUT n]])
#
# One CTest test per program, named `<suite>/<program>` -- and per optimization
# set on top of that, `<suite>/<program>:<opt-set>`, since those are separate
# runs that fail separately.
#
# XFAIL names the programs known to fail. The test still runs and CTest inverts
# its result, so one that starts passing is reported as a failure and the entry
# cannot outlive the bug. That is what it buys over dropping the program from
# TESTS, which reports nothing either way.
#
# This is deliberately not test-runner.exe driving a whole list. CTest's own
# scheduler then owns the parallelism, `ctest -R` addresses one program, and
# `--rerun-failed` re-runs the six that broke rather than all seven hundred.
# What test-runner did per child is small enough to reproduce inline: set
# MONO_PATH/MONO_CONFIG, hand the runtime `-O=<opt-set>` and the suite's
# runtime arguments, and compare the exit code. TEST_DRIVER_TIMEOUT_SEC still
# carries the per-test timeout, as it did on each child test-runner spawned.
function(mono_runtime_suite name)
  cmake_parse_arguments(ARG "" "LABEL;RUNTIME_ARGS;OPT_SETS;TIMEOUT;EXPECT;WORKDIR;PROCESSORS;LONG_TIMEOUT"
                            "TESTS;ENV;GC;SKIP_BOEHM;LONG;XFAIL" ${ARGN})
  if(NOT ARG_TESTS)
    return()
  endif()

  # A TESTS entry nothing builds fails at run time instead of at configure
  # time. GENERATED is the property CMake sets on every add_custom_command ()
  # OUTPUT, however it was produced, so this needs no list of its own.
  foreach(_t IN LISTS ARG_TESTS)
    if(_t MATCHES "\\.(exe|dll)$")
      get_source_file_property(_t_generated "${CMAKE_CURRENT_BINARY_DIR}/${_t}" GENERATED)
      if(NOT _t_generated)
        message(FATAL_ERROR
          "mono_runtime_suite(${name}): '${_t}' is not built by any "
          "add_custom_command () in this directory.")
      endif()
    endif()
  endforeach()

  if(NOT ARG_GC)
    set(ARG_GC ${_mono_gcs})
  else()
    # A suite can ask for a collector this build did not produce. Drop it
    # rather than emitting a test that cannot run.
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
        string(REGEX REPLACE "[^A-Za-z0-9._-]" "_" _tmpdir "${_gname}")
        set(_tmpdir "${_tmp_root}/${_tmpdir}")
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
                         "-DMONO_TIMEOUT_BINARY=${MONO_TIMEOUT_BINARY}"
                         "-DMONO_TEST_TMPDIR=${_tmpdir}"
                         -P "${_run_test}" --
                         ${_wrapper} ${_oarg} ${_rt_args} "${_test}"
                 WORKING_DIRECTORY "${ARG_WORKDIR}")
        set_tests_properties("${_gname}" PROPERTIES
          LABELS "${ARG_LABEL}"
          TIMEOUT ${_ctest_timeout})
        if(_test IN_LIST ARG_XFAIL)
          set_tests_properties("${_gname}" PROPERTIES WILL_FAIL TRUE)
        endif()
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

# The LLVM backend cannot tail-call these Win64 cases once an argument is
# passed on the stack. In the mixed-tier suite, promotion during recursion
# makes the outcome timing-dependent. Run them separately with promotion
# disabled and keep the expected failures in the backend-only suite.
set(_conservestack_xfail "")
set(_conservestack_pinned "")
if(WIN32)
  foreach(_n RANGE 16 52)
    list(APPEND _conservestack_xfail "tailcall/interface-conservestack/${_n}.exe")
    list(APPEND _conservestack_pinned "tailcall/interface-conservestack/${_n}.exe")
  endforeach()
endif()

# The suites
#
# The corpus default is 300s, which the three below have been measured getting
# uncomfortably close to on a machine with its cores busy. Each is slow for a
# reason its neighbours are not, so each gets the long budget rather than the
# whole corpus being loosened to cover them.
#
#   dynamic-method-churn  asks the JIT for 40000 compiles -- 20000 dynamic
#     methods, each with a runtime-invoke wrapper of its own, since a dynamic
#     method cannot share the cached one. That is minutes of LLVM at any
#     per-method cost this backend could plausibly reach. Measured at 412s.
#   appdomain-threadpool-unload  unloads 100 domains from a PLINQ query sized
#     to ProcessorCount, each with a thread-pool item spinning in it. It wants
#     the whole machine and gets a share of it, so its cost is set by what else
#     is running: 140s, 227s and 266s across runs, and killed at the 300s mark
#     on the boehm half of a full sweep.
#   appdomain-unload  creates and unloads domains with deliberately slow
#     finalizers and a 10s BeginInvoke still in flight, so most of its time is
#     spent waiting rather than running -- 171s to 247s measured, which is not
#     margin enough to leave at 300s.
#
# Tier 0 is off, so every method here goes through the backend. Most of these
# programs run their body once, which is too few calls to spend a counter, so at
# the default tier they would test classic tier 0 instead of the backend -- the
# tier-0 arm below is where that configuration is covered.
mono_runtime_suite(runtime TESTS ${_regular}
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier0-filter=0"
                   SKIP_BOEHM ${MONO_TESTS_BOEHM_DISABLED}
                   XFAIL ${_conservestack_xfail}
                   LONG dynamic-method-churn.exe
                        appdomain-unload.exe
                        appdomain-threadpool-unload.exe
                        bug-18026.exe)

# CoreCLR's tailcall corpus, run like any other program: several of these
# recurse deeply enough that a missed tail call is a stack overflow rather than
# a subtle difference, so running them is the check.
mono_runtime_suite(runtime-tailcall TESTS ${_tailcall}
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier0-filter=0")

mono_runtime_suite(gshared LABEL gshared TESTS ${_gshared})

# Every allocation is kept while sequence points are on, because a debugger stops
# at one and can hand any object a frame holds to a method it is asked to call.
# allocation_is_observable () answers yes for every class there, so this arm is
# the one that reaches mono.alloc.object.kept for a class nothing else marks, and
# the only one that reaches mono.alloc.vector.kept at all.
mono_runtime_suite(runtime-alloc-kept TESTS alloc-elide.exe
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier0-filter=0" "MONO_DEBUG=gen-seq-points")

# GVN and DSE read allockind(zeroed), and tier 1 runs neither. The threshold is
# low enough that each arm promotes inside the loop in Main.
mono_runtime_suite(runtime-alloc-zeroed TESTS alloc-elide.exe
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=100000")

# The copy a value type with references moves as, and the cards behind it.
# check-remset-consistency walks the old heap at each minor collection and
# aborts on an old-to-young reference no remembered set holds, which is what a
# missing card leaves behind. SGen only -- Boehm parses MONO_GC_DEBUG itself and
# knows only do-not-finalize and log-finalizers, so it would run this arm with
# no check at all. The threshold reaches tier 2, where the copy meets SROA and
# the dead-allocation walk. The arms are short enough that the program ends
# first at the default.
mono_runtime_suite(runtime-value-copy TESTS value-copy.exe GC sgen
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=100000"
                       "MONO_GC_DEBUG=check-remset-consistency")

# Continuations, whose two outcomes want naming rather than accepting either.
# With a collector that keeps the saved stack out of the heap they work, at the
# default tier as well as with everything compiled: a classic frame is a native
# frame like any other. Boehm is the collector that does not.
mono_runtime_suite(runtime-tasklets TESTS tasklets.exe GC sgen
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier0-filter=0" "MONO_TEST_TASKLETS=run")
mono_runtime_suite(runtime-tasklets-boehm TESTS tasklets.exe GC boehm
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier0-filter=0" "MONO_TEST_TASKLETS=refuse")
mono_runtime_suite(runtime-tasklets-tier0 LABEL tier0 TESTS tasklets.exe GC sgen
                   ENV "MONO_TEST_TASKLETS=run")

# Regression test for a shutdown hang: mono_thread_manage_internal ()'s abort
# phase used to wait forever for a background thread that a suspend signal
# never caught inside managed code. main-returns-background-tight-loop.exe is
# already in the general corpus above at its 300s budget. This copy's 150s
# catches a hang in minutes instead of the 900s the original bug ran to.
mono_runtime_suite(runtime-shutdown-background-abort
                   TESTS main-returns-background-tight-loop.exe
                   TIMEOUT 150)

# The whole corpus at the default tier: each method starts in the classic
# compiler and the hot ones are compiled by the backend underneath it, so the
# two engines are in one process and a method can change engine while its
# callers are running. This is the tier every program starts in, so it gets a
# label of its own rather than sitting in `runtime`, and anything that cannot
# run here belongs in MONO_TESTS_CLASSIC_TIER0_DISABLED, with the reason.
#
# check-remset-consistency for the reason mini-regression-tier0 carries it: a
# store compiled without its write barrier is caught at the store instead of
# corrupting some later collection.
set(_tier0 ${_regular})
list(REMOVE_ITEM _tier0 ${MONO_TESTS_CLASSIC_TIER0_DISABLED})
if(_conservestack_pinned)
  list(REMOVE_ITEM _tier0 ${_conservestack_pinned})
endif()
mono_runtime_suite(runtime-tier0 LABEL tier0 TESTS ${_tier0}
                   ENV "MONO_GC_DEBUG=check-remset-consistency"
                   SKIP_BOEHM ${MONO_TESTS_BOEHM_DISABLED}
                   LONG appdomain-threadpool-unload.exe
                        dynamic-method-churn.exe
                        appdomain-unload.exe
                        bug-18026.exe)

# Run the excluded cases without tier promotion.
mono_runtime_suite(runtime-conservestack-tier0 LABEL tier0
                   TESTS ${_conservestack_pinned}
                   ENV "MONO_GC_DEBUG=check-remset-consistency"
                       "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier1-threshold=0")

# The tailcall corpus at the default tier, where a tail site is the classic
# compiler's rather than the backend's. Both engines have to honour the same
# shapes, and a program that alternates between them has to hop without growing
# the stack.
mono_runtime_suite(runtime-tailcall-tier0 LABEL tier0 TESTS ${_tailcall})

# domain-stress runs the appdomain create/unload loop for a fixed iteration
# count rather than a fixed duration, so its wall time is whatever the machine
# gives it. A full run has been measured at 816s of the suite's 900s, and it has
# been killed at the 900s mark on both collectors -- which reports as a plain
# failure, not a timeout, because it is the driver that does the killing.
mono_runtime_suite(runtime-stress LABEL stress TESTS ${_stress} TIMEOUT 900
                   LONG domain-stress.exe LONG_TIMEOUT 1800)
mono_runtime_suite(runtime-process-stress LABEL stress TESTS ${_stress_process} TIMEOUT 900)

# The SGen matrix. Each collector configuration is its own test, so a failure
# names the mode rather than just "sgen". The argument strings are verbatim
# from the automake recipes: the collector is selected on the command line and
# not through MONO_GC_PARAMS, and the toggleref and bridge suites need their
# test hooks (`toggleref-test`, `--gc-debug=bridge=...`) switched on or the
# behaviour they check never happens.
#
# PROCESSORS: these are GC stress programs, and most of the configurations run a
# concurrent or parallel collector, so one of them is worth several ordinary
# tests to the scheduler. Without the hint CTest packs `-j` of them alongside
# everything else and starves the rest.
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

# A store from one old object to another marks its card only while a concurrent
# collection runs. The code that decides has no other exerciser.
#
# Under mod-union-consistency-check the collector reports a card the store
# missed, at the next collection and by the object it belongs to. Without the
# option the same miss is a payload the collection frees, and the program then
# reads a wrong value on the runs where the timing lands that way.
#
# One program rather than the whole list, because the check walks the heap at
# every collection.
mono_runtime_suite(sgen-wbarrier-mod-union LABEL sgen GC sgen TESTS sgen-wbarrier.exe
                   RUNTIME_ARGS "--gc=sgen --gc-debug=mod-union-consistency-check --gc-params=major=marksweep-conc,minor=simple"
                   TIMEOUT 900 PROCESSORS 4)

# One-off suites. These are not test-runner corpora: each is a single program
# whose exit code or output is the result.
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
  COMMAND ${_wrapper} load-exceptions.exe)
mono_runtime_check(runtime-multi-netmodule
  COMMAND ${_wrapper} test-multi-netmodule-4-exe.exe)
mono_runtime_check(runtime-cattr-type-load
  COMMAND ${_wrapper} custom-attr-errors.exe)
mono_runtime_check(runtime-reflection-load-with-context
  COMMAND ${_wrapper} reflection-load-with-context.exe)
mono_runtime_check(runtime-iomap-regression
  COMMAND ${_wrapper} exists.exe ENV "MONO_IOMAP=all")
# The four unhandled-exception suites `check-local` ran: the exit code the
# runtime produces for an unhandled exception, with and without a managed
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
# automake had these two target names the other way round. The names here
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
#
# Windows has no forced unwind: the mechanism the suite is about belongs to the
# Itanium C++ ABI, and the personality routine it drives is never reached here.
if(NOT WIN32)
  _mono_exe_list(_forced_unwind ${MONO_TESTS_FORCED_UNWIND_SRC})
  mono_runtime_suite(runtime-forced-unwind TESTS ${_forced_unwind} EXPECT 42)
endif()

# A managed value type of each size class crossing every boundary between
# classic tier 0, tier 1, and a delegate's invoke wrapper.
mono_runtime_suite(runtime-tier0-classic-struct-abi-all
                   TESTS tier0-classic-struct-abi.exe)

# A value-type return gathered out of registers, one struct per shape of the
# placement.
mono_runtime_suite(runtime-tier0-classic-ret-regs-all
                   TESTS tier0-classic-ret-regs.exe)

# Array.UnsafeMov, whose caller is in corlib.
mono_runtime_suite(runtime-tier0-classic-unsafe-mov-all
                   TESTS tier0-classic-unsafe-mov.exe)

# ThresholdZeroSpin (), called a thousand times with a loop of its own, under
# the threshold every other suite here reaches for when it wants a method
# pinned at tier 0.
mono_runtime_suite(runtime-tier0-classic-threshold-zero
                   TESTS tier0-classic-threshold-zero.exe
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier1-threshold=0")

# A value type whose scalars spend the whole integer parameter file, so the
# hidden return pointer behind the first argument lands in a stack slot. The
# threshold is zero so the test's own PromoteNow calls decide which side of
# each call is at tier 1, not a call count racing the compile queue.
mono_runtime_suite(runtime-tier0-classic-vret-spill-all
                   TESTS tier0-classic-vret-spill.exe
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier1-threshold=0")

# An explicit layout whose overlapping fields classic tier 0 and tier 1 have
# to resolve the same way, passed across the seam both bare and behind a field
# of its own.
mono_runtime_suite(runtime-tier0-classic-union-abi-all
                   TESTS tier0-classic-union-abi.exe
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier1-threshold=0")

# CheckClassic asserts several methods' tiers at once, which only holds once
# Main () and each of them compiles through classic.
mono_runtime_suite(runtime-tier0-classic-simd-abi
                   TESTS tier0-classic-simd-abi.exe
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier1-threshold=0"
                       "MONO_TIER0_CLASSIC_ALL=1")

# A class initializer that only tier 0 can run, reached through a static call,
# through an AggressiveInlining callee and through a static field.
mono_runtime_suite(runtime-tier0-classic-class-init-all
                   TESTS tier0-classic-class-init.exe)

# A shared classic caller naming its own open type argument as the type
# argument of the class its callee is declared in, both compiled through
# classic tier 0.
mono_runtime_suite(runtime-tier0-classic-open-callee-all
                   TESTS tier0-classic-open-callee.exe)

# The tier-2 cost model. Its root has to gather counts at tier 1 and then be
# compiled at tier 2 once, on the thread that asks - so self-promotion is turned
# off and the test drives the compile itself.
_mono_exe_list(_tier2_costed ${MONO_TESTS_TIER2_COSTED_SRC})
mono_runtime_suite(runtime-tier2-costed TESTS ${_tier2_costed}
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0")

# The work half of the tier-2 counter, which promotes a body the calls alone
# never reach. The threshold is named here rather than left at its default, so
# the kernel spends it in the calls the test makes instead of in a run long
# enough to measure. The test carries its own control, a second kernel that takes
# the same calls and does far less work, so both arms assert that one. The second
# arm turns automatic promotion off and asserts that neither kernel moves.
_mono_exe_list(_tier2_cost_trigger ${MONO_TESTS_TIER2_COST_TRIGGER_SRC})
mono_runtime_suite(runtime-tier2-cost-trigger TESTS ${_tier2_cost_trigger}
                   ENV "MONO_WANT_TIER2=on"
                       "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=10000000")
mono_runtime_suite(runtime-tier2-cost-trigger-off TESTS ${_tier2_cost_trigger}
                   ENV "MONO_WANT_TIER2=off"
                       "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0")

# Entering a delegate's target directly. Two arms, on and off, the way
# runtime-tier2-cost-trigger has it. The root drives its own compiles, so
# self-promotion is off: what the test reads is the tier it asked for.
_mono_exe_list(_eliminate_delegate ${MONO_TESTS_ELIMINATE_DELEGATE_SRC})
mono_runtime_suite(runtime-eliminate-delegate TESTS ${_eliminate_delegate}
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0")
mono_runtime_suite(runtime-eliminate-delegate-off TESTS ${_eliminate_delegate}
                   ENV "MONO_ELIMINATE_DELEGATES=off"
                       "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-eliminate-delegates=0")

# An object address folded into compiled code out of a readonly static, against a
# collector that then moves the object. SGen only: nothing moves under Boehm, so
# that arm would pass whatever the fold wrote down. Both thresholds are zero, so
# the only compile is the one the test asks for by name. An automatic promotion
# landing after the collection would fold the object's current, already-moved
# address, so the bug would have nothing to show. One arm per compiled tier,
# because each runs the pass in a pipeline of its own.
_mono_exe_list(_static_const_move ${MONO_TESTS_STATIC_CONST_MOVE_SRC})
mono_runtime_suite(runtime-static-const-move TESTS ${_static_const_move} GC sgen
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier1-threshold=0 --llvm-opt=-mono-tier2-threshold=0")
mono_runtime_suite(runtime-static-const-move-tier1 TESTS ${_static_const_move} GC sgen
                   ENV "MONO_STATIC_CONST_TIER=1"
                       "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier1-threshold=0 --llvm-opt=-mono-tier2-threshold=0")

# Exercise readonly static reference loads and null checks at tier 2.
_mono_exe_list(_initonly_nullness ${MONO_TESTS_INITONLY_NULLNESS_SRC})
mono_runtime_suite(runtime-initonly-nullness TESTS ${_initonly_nullness}
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier1-threshold=0 --llvm-opt=-mono-tier2-threshold=0")

# Object.GetHashCode ()'s fast path. Two arms, on and off, the way
# runtime-eliminate-delegate has it. The root drives its own compiles, so
# self-promotion is off: what the test reads is the tier it asked for.
_mono_exe_list(_hash_fastpath ${MONO_TESTS_HASH_FASTPATH_SRC})
mono_runtime_suite(runtime-hash-fastpath TESTS ${_hash_fastpath}
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier1-threshold=0 --llvm-opt=-mono-tier2-threshold=0")
mono_runtime_suite(runtime-hash-fastpath-off TESTS ${_hash_fastpath}
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier1-threshold=0 --llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-hash-fastpath=0")

# Each SIMD operation is computed at tier 0 and at both compiled tiers, where
# the backend's written body runs instead of tier 0's IL. The tier-1 and tier-2
# thresholds are zero so the test's own PromoteNow calls decide which tier ran,
# not a call count racing the compile queue.
mono_runtime_suite(runtime-simd-semantics TESTS simd-semantics.exe
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier1-threshold=0 --llvm-opt=-mono-tier2-threshold=0")

# The off arm runs the same program with the lowering off, so every tier is back
# on the managed IL. It is the negative control: the answers must not move when
# the written bodies come out.
mono_runtime_suite(runtime-simd-semantics-off TESTS simd-semantics.exe
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier1-threshold=0 --llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-simd=0")

# The guard a dispatch on an array receiver goes through. It is tier 2's, and
# the default threshold is far past what this program runs, so the arm that
# reaches it names one of its own. The off arm leaves every such site
# dispatching, which is the answer the guard has to agree with.
mono_runtime_suite(runtime-array-guard TESTS array-devirt.exe
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=10000")
mono_runtime_suite(runtime-array-guard-off TESTS array-devirt.exe
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=10000 --llvm-opt=-mono-guard-arrays=0")

# Compare tier-2 IRCE with the same kernels compiled without the pass.
mono_runtime_suite(runtime-irce TESTS irce-bounds.exe)
mono_runtime_suite(runtime-irce-off TESTS irce-bounds.exe
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-irce=0")

# The guess GuardDispatchPass takes on a receiver it cannot prove a class
# for, behind the same array rule's guard. Tier 2 only, same as the array
# arm above, and the same threshold: this file spends the same shape of
# calls array-devirt.cs does to reach it. The off arm leaves every such site
# dispatching, which is the answer the guess has to agree with.
mono_runtime_suite(runtime-class-guard TESTS class-devirt.exe
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=10000")
mono_runtime_suite(runtime-class-guard-off TESTS class-devirt.exe
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=10000 --llvm-opt=-mono-guard-classes=0")

# The faulting-access rewrite MonoNullCheckFaultPass makes of a null check
# ImplicitNullChecks itself declined to fold. implicit-null-checks.cs drives
# its own tiers through PromoteNow, so this needs no threshold of its own,
# only the off arm: it leaves every such check as the compare and branch
# ImplicitNullChecks left standing, which is the answer the rewrite has to
# agree with.
mono_runtime_suite(runtime-fault-null-checks-off TESTS implicit-null-checks.exe
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-fault-null-checks=0")

# A bare type parameter's cast site resolves its target class through the
# rgctx regardless of tier, so tier 0 is off rather than raised -- the point
# is to reach the backend at all, not to reach a particular tier of it. The
# off arm falls every such site back to the cached probe, which is the answer
# the resolved depth has to agree with.
mono_runtime_suite(runtime-shared-cast-depth TESTS shared-cast-depth.exe
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier0-filter=0")
mono_runtime_suite(runtime-shared-cast-depth-off TESTS shared-cast-depth.exe
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier0-filter=0 --llvm-opt=-mono-rgctx-cast-depth=0")

# Exercise invariant element_class loads after Sum () reaches tier 2.
mono_runtime_suite(runtime-unbox-element-class TESTS unbox-element-class.exe
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=10000")

# gshared-boehm-alloc-shape.cs's shared bodies have to reach the backend on
# their first call, which the default tier never does: classic tier 0 builds
# a vtable through mono_class_vtable () instead of asking either allocation
# site for a shape. The `gshared` suite runs this corpus at the default
# tier, so it is not this arm's negative control -- neither one reaches the
# code this gates.
mono_runtime_suite(runtime-gshared-alloc-shape TESTS gshared-boehm-alloc-shape.exe
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier0-filter=0")

# boehm-domain-alloc-shape.cs's allocation has to reach the backend on its
# first call, in the second AppDomain it runs the test in, which the default
# tier does not: classic tier 0 builds a vtable through mono_class_vtable ()
# instead of asking emit_object_alloc () for a shape.
mono_runtime_suite(runtime-boehm-domain-alloc-shape TESTS boehm-domain-alloc-shape.exe
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier0-filter=0")

# The bonuses mono adds to the cost model. Two arms, on and off, the way
# runtime-tier2-cost-trigger has it. The root drives its own compiles, so
# self-promotion is off the way the costed suite has it, and the trivial
# pre-pass is off so that every fold the test reads is the cost model's.
# The threshold and the calibration behind it are the test file's subject.
_mono_exe_list(_tier2_inline_policy ${MONO_TESTS_TIER2_INLINE_POLICY_SRC})
mono_runtime_suite(runtime-tier2-inline-policy TESTS ${_tier2_inline_policy}
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-inline-il-limit=0 --llvm-opt=-mono-inline-cold-callsite-threshold=110")
mono_runtime_suite(runtime-tier2-inline-policy-off TESTS ${_tier2_inline_policy}
                   ENV "MONO_INLINE_POLICY=off"
                       "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-inline-il-limit=0 --llvm-opt=-mono-inline-cold-callsite-threshold=110 --llvm-opt=-mono-inline-devirt-return-bonus=0 --llvm-opt=-mono-inline-devirt-arg-bonus=0 --llvm-opt=-mono-inline-scalarize-arg-bonus=0 --llvm-opt=-mono-inline-dispatch-is-a-load=false --llvm-opt=-mono-inline-eliminate-vtable-fields=false --llvm-opt=-mono-inline-noreturn-free=false")

# The devirt-arg bonus's own two gates, too narrow for a caller-side shape
# each. Two arms the same way, and the file says why it names a threshold of
# its own.
_mono_exe_list(_tier2_inline_arg_shapes ${MONO_TESTS_TIER2_INLINE_ARG_SHAPES_SRC})
mono_runtime_suite(runtime-tier2-inline-arg-shapes TESTS ${_tier2_inline_arg_shapes}
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-inline-il-limit=0 --llvm-opt=-mono-inline-cold-callsite-threshold=145")
mono_runtime_suite(runtime-tier2-inline-arg-shapes-off TESTS ${_tier2_inline_arg_shapes}
                   ENV "MONO_INLINE_POLICY=off"
                       "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-inline-il-limit=0 --llvm-opt=-mono-inline-cold-callsite-threshold=145 --llvm-opt=-mono-inline-devirt-arg-bonus=0 --llvm-opt=-mono-inline-noreturn-free=false")

# The return bonus on a callee that forwards a sealed-return call's answer
# rather than allocating its own. Two arms the same way, and the file says
# why it names a threshold of its own.
_mono_exe_list(_tier2_inline_return_forward ${MONO_TESTS_TIER2_INLINE_RETURN_FORWARD_SRC})
mono_runtime_suite(runtime-tier2-inline-return-forward TESTS ${_tier2_inline_return_forward}
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-inline-il-limit=0 --llvm-opt=-mono-inline-cold-callsite-threshold=80")
mono_runtime_suite(runtime-tier2-inline-return-forward-off TESTS ${_tier2_inline_return_forward}
                   ENV "MONO_INLINE_POLICY=off"
                       "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-inline-il-limit=0 --llvm-opt=-mono-inline-cold-callsite-threshold=80 --llvm-opt=-mono-inline-devirt-return-bonus=0 --llvm-opt=-mono-inline-noreturn-free=false")

# The return bonus on a callee that answers with `this` rather than allocating
# or forwarding a call. Two arms the same way, and the file says why it names
# a threshold of its own.
_mono_exe_list(_tier2_inline_return_self ${MONO_TESTS_TIER2_INLINE_RETURN_SELF_SRC})
mono_runtime_suite(runtime-tier2-inline-return-self TESTS ${_tier2_inline_return_self}
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-inline-il-limit=0 --llvm-opt=-mono-inline-cold-callsite-threshold=45")
mono_runtime_suite(runtime-tier2-inline-return-self-off TESTS ${_tier2_inline_return_self}
                   ENV "MONO_INLINE_POLICY=off"
                       "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-inline-il-limit=0 --llvm-opt=-mono-inline-cold-callsite-threshold=45 --llvm-opt=-mono-inline-devirt-return-bonus=0 --llvm-opt=-mono-inline-noreturn-free=false")

# Whether the cost model inlines a clause-bearing callee once its clause is
# dead. PromoteNow drives the compiles, so self-promotion is off, and the
# trivial pre-pass is off so an inline the test reads is never that one's
# instead.
_mono_exe_list(_tier2_inline_clause ${MONO_TESTS_TIER2_INLINE_CLAUSE_SRC})
mono_runtime_suite(runtime-tier2-inline-clause TESTS ${_tier2_inline_clause}
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-inline-il-limit=0")
mono_runtime_suite(runtime-tier2-inline-clause-off TESTS ${_tier2_inline_clause}
                   ENV "MONO_INLINE_CLAUSES=off"
                       "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-inline-il-limit=0 --llvm-opt=-mono-inline-clauses=0")

# PromoteNow drives the compiles, so self-promotion is off. Every callee here
# carries a clause, and is_small_and_clause_free () refuses those outright.
# The il-limit pin only guards a callee added here later that does not.
_mono_exe_list(_tier2_finally_guard_owner ${MONO_TESTS_TIER2_FINALLY_GUARD_OWNER_SRC})
mono_runtime_suite(runtime-tier2-finally-guard-owner TESTS ${_tier2_finally_guard_owner}
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-inline-il-limit=0")

# Whether a candidate the cost model materializes is weighed on its own
# tier-1 record or the root's. The suite drives both promotions itself, so
# self-promotion is off, and the trivial pre-pass is off so the inline the
# test reads is never that one's instead.
_mono_exe_list(_tier2_inline_profile_context ${MONO_TESTS_TIER2_INLINE_PROFILE_CONTEXT_SRC})
mono_runtime_suite(runtime-tier2-inline-profile-context TESTS ${_tier2_inline_profile_context}
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-inline-il-limit=0")

# Whether the cost model specializes a concrete callee out of a root that is
# itself shared (task #345). PromoteNow drives the compiles, so self-promotion
# is off, and the trivial pre-pass is off so the inline the test reads is the
# cost model's rather than that pass's.
_mono_exe_list(_tier2_inline_generic_scope ${MONO_TESTS_TIER2_INLINE_GENERIC_SCOPE_SRC})
mono_runtime_suite(runtime-tier2-inline-generic-scope TESTS ${_tier2_inline_generic_scope}
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-inline-il-limit=0")

# Disable the pre-pass and use a 128-byte cost limit at every call-site
# temperature. Each guarded body exceeds the limit before dead blocks are
# removed and fits after its condition is resolved.
_mono_exe_list(_tier2_inline_typeof_guard ${MONO_TESTS_TIER2_INLINE_TYPEOF_GUARD_SRC})
mono_runtime_suite(runtime-tier2-inline-typeof-guard TESTS ${_tier2_inline_typeof_guard}
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-inline-il-limit=0 --llvm-opt=-mono-inline-cost-il-limit=128 --llvm-opt=-mono-inline-cost-il-limit-hot=0 --llvm-opt=-mono-inline-cost-il-limit-cold=0")

# A filter-bearing callee can never inline -- getInlineCost refuses its
# llvm.localescape outright -- so both arms expect the same refusal.
# PromoteNow drives the compiles the same way runtime-tier2-inline-clause does.
_mono_exe_list(_tier2_inline_filter ${MONO_TESTS_TIER2_INLINE_FILTER_SRC})
mono_runtime_suite(runtime-tier2-inline-filter TESTS ${_tier2_inline_filter}
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-inline-il-limit=0")
mono_runtime_suite(runtime-tier2-inline-filter-off TESTS ${_tier2_inline_filter}
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-inline-il-limit=0 --llvm-opt=-mono-inline-clauses=0")

# The raising arm mono-inline-implicit-null-free leaves out of a callee's
# cost. The body is past the default cost-translate limit and past the
# default cold-callsite threshold, so both arms raise both -- the file says
# why. Its site is fallback-hot (Root () carries no loop), so both arms also
# pin -hot/-cold to 0 to hold the flat 512 this suite is calibrated against;
# #349's landing added this, the same isolation shape #342's own landing
# added elsewhere.
_mono_exe_list(_tier2_inline_nullcheck ${MONO_TESTS_TIER2_INLINE_NULLCHECK_SRC})
mono_runtime_suite(runtime-tier2-inline-nullcheck TESTS ${_tier2_inline_nullcheck}
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-inline-il-limit=0 --llvm-opt=-mono-inline-cost-il-limit=512 --llvm-opt=-mono-inline-cost-il-limit-hot=0 --llvm-opt=-mono-inline-cost-il-limit-cold=0 --llvm-opt=-mono-inlinedefault-threshold=1200 --llvm-opt=-mono-inline-cold-callsite-threshold=700")
mono_runtime_suite(runtime-tier2-inline-nullcheck-off TESTS ${_tier2_inline_nullcheck}
                   ENV "MONO_INLINE_POLICY=off"
                       "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-inline-il-limit=0 --llvm-opt=-mono-inline-cost-il-limit=512 --llvm-opt=-mono-inline-cost-il-limit-hot=0 --llvm-opt=-mono-inline-cost-il-limit-cold=0 --llvm-opt=-mono-inlinedefault-threshold=1200 --llvm-opt=-mono-inline-cold-callsite-threshold=700 --llvm-opt=-mono-inline-implicit-null-free=false --llvm-opt=-mono-inline-noreturn-free=false")

# The raising arm mono-inline-noreturn-free leaves out of a callee's cost, on
# a guard that raises through an ordinary comparison rather than a folded
# null check. The default cold-callsite threshold already sits between the
# two costs, so neither arm needs one of its own -- the file says what they
# measured. Its site is fallback-hot too, so both arms pin -hot/-cold to 0 to
# hold the flat -mono-inline-cost-il-limit default (256) this suite is
# calibrated against, the same reason tier2-inline-nullcheck above needs it.
_mono_exe_list(_tier2_inline_noreturn ${MONO_TESTS_TIER2_INLINE_NORETURN_SRC})
mono_runtime_suite(runtime-tier2-inline-noreturn TESTS ${_tier2_inline_noreturn}
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-inline-il-limit=0 --llvm-opt=-mono-inline-cost-il-limit-hot=0 --llvm-opt=-mono-inline-cost-il-limit-cold=0")
mono_runtime_suite(runtime-tier2-inline-noreturn-off TESTS ${_tier2_inline_noreturn}
                   ENV "MONO_INLINE_POLICY=off"
                       "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-inline-il-limit=0 --llvm-opt=-mono-inline-cost-il-limit-hot=0 --llvm-opt=-mono-inline-cost-il-limit-cold=0 --llvm-opt=-mono-inline-noreturn-free=false")

# mono-inline-cost-il-limit-hot/-cold, on the same body under two names at a
# hot and a cold site. One arm: the file's own comment says why no second one
# is needed -- the hot/cold contrast inside this one run is what the flag is
# for.
_mono_exe_list(_tier2_inline_heat_il_limit ${MONO_TESTS_TIER2_INLINE_HEAT_IL_LIMIT_SRC})
mono_runtime_suite(runtime-tier2-inline-heat-il-limit TESTS ${_tier2_inline_heat_il_limit}
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-inline-il-limit=0 --llvm-opt=-mono-inline-cost-il-limit=0 --llvm-opt=-mono-inline-cost-il-limit-hot=500 --llvm-opt=-mono-inline-cost-il-limit-cold=100")

# mono-inline-trivial-fanout-limit and mono-inline-trivial-instance-budget,
# each tight enough that the file's four callees settle both in one run: the
# file's own comment says which callee each limit is the one that catches.
_mono_exe_list(_trivial_inline_fanout ${MONO_TESTS_TRIVIAL_INLINE_FANOUT_SRC})
mono_runtime_suite(runtime-trivial-inline-fanout TESTS ${_trivial_inline_fanout}
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-inline-trivial-fanout-limit=6 --llvm-opt=-mono-inline-trivial-instance-budget=10")

# mono-inline-cost-byte-budget, on two 113-byte candidates the count budget
# alone would let both through. 150 admits one and leaves too little for the
# other; re-measure both if this starts failing:
#
#   MONO_LLVM_JIT_TRACE=1 mono-sgen --llvm-opt=-mono-tier2-threshold=0 \
#     --llvm-opt=-mono-inline-il-limit=0 --llvm-opt=-mono-inline-cost-il-limit-hot=10 \
#     tier2-inline-byte-budget.exe
#
# names each candidate's own size ("N IL bytes over the limit of 10"); the off
# arm sets the byte budget far past what both together spend, so it is no
# longer the binding constraint and the count budget's own default (32) is.
_mono_exe_list(_tier2_inline_byte_budget ${MONO_TESTS_TIER2_INLINE_BYTE_BUDGET_SRC})
mono_runtime_suite(runtime-tier2-inline-byte-budget TESTS ${_tier2_inline_byte_budget}
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-inline-il-limit=0 --llvm-opt=-mono-inline-cost-byte-budget=150")
mono_runtime_suite(runtime-tier2-inline-byte-budget-off TESTS ${_tier2_inline_byte_budget}
                   ENV "MONO_INLINE_POLICY=off"
                       "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-inline-il-limit=0 --llvm-opt=-mono-inline-cost-byte-budget=1000000")

# carries_an_elision_candidate (), on two ~113-byte constructors at cold
# sites -- past the flat 64-byte cold default, under the 256-byte ordinary
# one. One arm: an unescaped and an escaping candidate settle the question
# inside the same run, the same reason tier2-inline-heat-il-limit's own
# single arm suffices.
_mono_exe_list(_tier2_inline_cold_elision ${MONO_TESTS_TIER2_INLINE_COLD_ELISION_SRC})
mono_runtime_suite(runtime-tier2-inline-cold-elision TESTS ${_tier2_inline_cold_elision}
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-inline-il-limit=0")

# The two alloc-elision bonuses, on a callee that only reads an uncaptured
# argument's fields. The off arm leaves the other bonuses alone, because what
# it separates is this pair from the fold tier2-inline-policy.cs's Measure ()
# takes with the argument bonus instead. The threshold is sized for the full
# bonus alone -- the file says why -- so the pending bonus's own site
# declines on both arms too. Its site is fallback-hot, so both arms pin
# -hot/-cold to 0 to hold the flat -mono-inline-cost-il-limit default (256)
# the threshold above is calibrated against.
_mono_exe_list(_tier2_inline_alloc_elision ${MONO_TESTS_TIER2_INLINE_ALLOC_ELISION_SRC})
mono_runtime_suite(runtime-tier2-inline-alloc-elision TESTS ${_tier2_inline_alloc_elision}
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-inline-il-limit=0 --llvm-opt=-mono-inline-cost-il-limit-hot=0 --llvm-opt=-mono-inline-cost-il-limit-cold=0 --llvm-opt=-mono-inline-cold-callsite-threshold=100")
mono_runtime_suite(runtime-tier2-inline-alloc-elision-off TESTS ${_tier2_inline_alloc_elision}
                   ENV "MONO_INLINE_POLICY=off"
                       "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-inline-il-limit=0 --llvm-opt=-mono-inline-cost-il-limit-hot=0 --llvm-opt=-mono-inline-cost-il-limit-cold=0 --llvm-opt=-mono-inline-cold-callsite-threshold=100 --llvm-opt=-mono-inline-alloc-elision-bonus=0 --llvm-opt=-mono-inline-alloc-elision-pending-bonus=0 --llvm-opt=-mono-inline-noreturn-free=false")

# The delegate-arg-bonus, on a callee that invokes a parameter the site fills
# with a delegate whose target the compile can name. The off arm leaves the
# other bonuses alone, because what it separates is this one from the class
# argument bonus tier2-inline-policy.cs's Measure () takes instead.
_mono_exe_list(_tier2_inline_delegate_arg ${MONO_TESTS_TIER2_INLINE_DELEGATE_ARG_SRC})
mono_runtime_suite(runtime-tier2-inline-delegate-arg TESTS ${_tier2_inline_delegate_arg}
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-inline-il-limit=0 --llvm-opt=-mono-inline-cold-callsite-threshold=165")
mono_runtime_suite(runtime-tier2-inline-delegate-arg-off TESTS ${_tier2_inline_delegate_arg}
                   ENV "MONO_INLINE_POLICY=off"
                       "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-inline-il-limit=0 --llvm-opt=-mono-inline-cold-callsite-threshold=165 --llvm-opt=-mono-inline-devirt-delegate-arg-bonus=0 --llvm-opt=-mono-inline-noreturn-free=false")

# What the cost model answers about a receiver the call site allocated. The off
# arm turns those answers off and leaves the bonuses alone, because what it
# separates is the fold rather than a threshold. Its site is fallback-hot, so
# both arms pin -hot/-cold to 0 to hold the flat 256 this suite is calibrated
# against.
_mono_exe_list(_tier2_inline_dispatch ${MONO_TESTS_TIER2_INLINE_DISPATCH_SRC})
mono_runtime_suite(runtime-tier2-inline-dispatch TESTS ${_tier2_inline_dispatch}
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-inline-il-limit=0 --llvm-opt=-mono-inline-cost-il-limit=256 --llvm-opt=-mono-inline-cost-il-limit-hot=0 --llvm-opt=-mono-inline-cost-il-limit-cold=0 --llvm-opt=-mono-inline-cold-callsite-threshold=190")
mono_runtime_suite(runtime-tier2-inline-dispatch-off TESTS ${_tier2_inline_dispatch}
                   ENV "MONO_INLINE_POLICY=off"
                       "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-inline-il-limit=0 --llvm-opt=-mono-inline-cost-il-limit=256 --llvm-opt=-mono-inline-cost-il-limit-hot=0 --llvm-opt=-mono-inline-cost-il-limit-cold=0 --llvm-opt=-mono-inline-cold-callsite-threshold=190 --llvm-opt=-mono-inline-dispatch-is-a-load=false --llvm-opt=-mono-inline-eliminate-vtable-fields=false --llvm-opt=-mono-inline-noreturn-free=false")

# What the cost model answers about a type test over a parameter. The off arm
# turns that answer off alone, so what it separates is the answered cascade. A
# cold callsite's budget is the lower of the default threshold and the cold
# one, so both arms raise both. Its site is fallback-hot, so both arms also
# pin -hot/-cold to 0 to hold the flat 512 this suite is calibrated against.
_mono_exe_list(_tier2_inline_casts ${MONO_TESTS_TIER2_INLINE_CASTS_SRC})
mono_runtime_suite(runtime-tier2-inline-casts TESTS ${_tier2_inline_casts}
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-inline-il-limit=0 --llvm-opt=-mono-inline-cost-il-limit=512 --llvm-opt=-mono-inline-cost-il-limit-hot=0 --llvm-opt=-mono-inline-cost-il-limit-cold=0 --llvm-opt=-mono-inlinedefault-threshold=400 --llvm-opt=-mono-inline-cold-callsite-threshold=400")
mono_runtime_suite(runtime-tier2-inline-casts-off TESTS ${_tier2_inline_casts}
                   ENV "MONO_INLINE_POLICY=off"
                       "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-inline-il-limit=0 --llvm-opt=-mono-inline-cost-il-limit=512 --llvm-opt=-mono-inline-cost-il-limit-hot=0 --llvm-opt=-mono-inline-cost-il-limit-cold=0 --llvm-opt=-mono-inlinedefault-threshold=400 --llvm-opt=-mono-inline-cold-callsite-threshold=400 --llvm-opt=-mono-inline-answer-casts=false --llvm-opt=-mono-inline-noreturn-free=false")

# A wrapper inlined into its caller. The off arm leaves the cost model nothing to
# translate, which is what separates the inline from the frame: both arms assert
# that the wrapper still has a frame, and only the inline moves its offset onto the
# caller's. A cold callsite's budget is the lower of the default threshold and
# the cold one, so both arms raise both. Only the off arm needs -hot/-cold
# pinned to 0: it relies on -cost-il-limit=0 taking the cost model out of the
# inline entirely, and its fallback-hot site would otherwise put it back in at
# -hot's default. The on arm's own assertions (an inline happened, a frame moved)
# do not depend on the exact limit, so it is unaffected either way.
_mono_exe_list(_tier2_inline_wrapper ${MONO_TESTS_TIER2_INLINE_WRAPPER_SRC})
mono_runtime_suite(runtime-tier2-inline-wrapper TESTS ${_tier2_inline_wrapper}
                   ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-inline-il-limit=0 --llvm-opt=-mono-inlinedefault-threshold=400 --llvm-opt=-mono-inline-cold-callsite-threshold=400")
mono_runtime_suite(runtime-tier2-inline-wrapper-off TESTS ${_tier2_inline_wrapper}
                   ENV "MONO_WRAPPER_INLINE=off"
                       "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier2-threshold=0 --llvm-opt=-mono-inline-il-limit=0 --llvm-opt=-mono-inline-cost-il-limit=0 --llvm-opt=-mono-inline-cost-il-limit-hot=0 --llvm-opt=-mono-inline-cost-il-limit-cold=0 --llvm-opt=-mono-inlinedefault-threshold=400 --llvm-opt=-mono-inline-cold-callsite-threshold=400")

# MONO_ENV_OPTIONS has to reach the runtime before it parses its own argv.
foreach(_gc IN LISTS _mono_gcs)
  _mono_gc_env(_gc_env "${_gc}")
  add_test(NAME "runtime-env-options@${_gc}"
           COMMAND "${CMAKE_COMMAND}" -E env "MONO_PATH=${_class_dir}"
                   "${_gc_env}"
                   "MONO_ENV_OPTIONS=--version" ${_wrapper} array-init.exe
           WORKING_DIRECTORY "${_bin}")
  set_tests_properties("runtime-env-options@${_gc}" PROPERTIES
    LABELS runtime
    PASS_REGULAR_EXPRESSION "Architecture:")
endforeach()

# eglib's symbols are remapped to monoeg_* so that a runtime linked into a host
# that already has glib does not collide with it. Anything still exported as a
# bare g_* is a missed entry in eglib-remap.h.
# It is a property of each binary that was linked, so check each of them.
#
# Not on Windows: nm reads no symbol table out of a PE binary, because the names
# are in the PDB beside it. What a host there could collide with is the DLL's
# export table, which is a different question and a different tool.
if(MONO_NM AND NOT WIN32)
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

mono_runtime_suite(runtime-ignoresaccesschecks
  TESTS ignoresaccesschecks-test.exe)

# The second arm turns tier 0 off so the same accesses reach the compiled
# engine's checks. A refused access throws on its first call, so it never
# promotes on its own.
mono_runtime_suite(runtime-skipverification
  TESTS skipverification-test.exe skipverification-strict-test.exe)
mono_runtime_suite(runtime-skipverification-compiled
  TESTS skipverification-test.exe skipverification-strict-test.exe
  ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier0-filter=0")

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
# Some tier-0 arms name the invalid method rather than taking every method. That
# keeps its caller compiled, so the call goes through the stub the backend
# published and the backend is what answers for the callee. A classic caller's
# unrestricted arm below checks the same verdict without naming anything -
# every call it makes goes through the callee's thunk regardless.
function(_mono_verification_check name expect)
  cmake_parse_arguments(ARG "" "PROGRAM" "ARGS;ENV;REJECT" ${ARGN})
  if(NOT ARG_PROGRAM)
    set(ARG_PROGRAM verification-invalid-il.exe)
  endif()
  foreach(_gc IN LISTS _mono_gcs)
    _mono_gc_env(_gc_env "${_gc}")
    add_test(NAME "${name}@${_gc}"
             COMMAND "${CMAKE_COMMAND}" -E env "MONO_PATH=${_class_dir}"
                     "${_gc_env}" "MONO_LLVM_JIT_TRACE=1" ${ARG_ENV}
                     ${_wrapper} ${ARG_ARGS} "${ARG_PROGRAM}"
             WORKING_DIRECTORY "${_bin}")
    set_tests_properties("${name}@${_gc}" PROPERTIES
      LABELS runtime TIMEOUT 300
      PASS_REGULAR_EXPRESSION "${expect}"
      FAIL_REGULAR_EXPRESSION "${ARG_REJECT}")
  endforeach()
endfunction()

_mono_verification_check(runtime-verification-off "ran 42"
                         ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier0-filter=0" REJECT "rejected")
_mono_verification_check(runtime-verification-validil
                         "rejected System.InvalidProgramException"
                         ARGS --security=validil
                         ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier0-filter=0" REJECT "ran 42")
_mono_verification_check(runtime-verification-tier0
                         "compiling Probe:Unverifiable"
                         ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier0-filter=Probe:Unverifiable"
                         REJECT "rejected")
# Rejecting without ever printing the routing line is the placement itself: the
# verdict is reached before the tier is chosen.
_mono_verification_check(runtime-verification-validil-tier0
                         "rejected System.InvalidProgramException"
                         ARGS --security=validil
                         ENV "MONO_ENV_OPTIONS=--llvm-opt=-mono-tier0-filter=Probe:Unverifiable"
                         REJECT "ran 42|compiling Probe:Unverifiable")

# Tier 0 unrestricted.
_mono_verification_check(runtime-verification-validil-classic
                         "rejected System.InvalidProgramException"
                         ARGS --security=validil
                         REJECT "ran 42")

# The same, for a callee unverifiable under a different ECMA-335 rule.
_mono_verification_check(runtime-verification-inlined
                         "rejected System.Security.VerificationException"
                         PROGRAM verification-inlined-il.exe
                         ARGS --verify-all
                         REJECT "ran 42")
_mono_verification_check(runtime-verification-inlined-off "ran 42"
                         PROGRAM verification-inlined-il.exe
                         REJECT "rejected")
